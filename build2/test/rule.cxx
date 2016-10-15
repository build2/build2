// file      : build2/test/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/rule>

#include <build2/scope>
#include <build2/target>
#include <build2/algorithm>
#include <build2/diagnostics>

#include <build2/test/target>

#include <build2/test/script/parser>
#include <build2/test/script/runner>
#include <build2/test/script/script>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    struct match_data
    {
      bool test = false;
      bool script = false;
    };

    static_assert (sizeof (match_data) <= target::data_size,
                   "insufficient space");

    match_result rule::
    match (action a, target& t, const string&) const
    {
      match_data md;

      // We have two very different cases: testscript and simple test (plus it
      // may not be a testable target at all). So as the first step determine
      // which case this is. If we have any prerequisites of the test{} type,
      // then this is the testscript case.
      //
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (p.is_a<testscript> ())
        {
          md.script = true;
          break;
        }
      }

      if (md.script)
      {
        // We treat this target as testable unless the test variable is
        // explicitly set to false.
        //
        lookup l (t["test"]);
        md.test = !l || cast<path> (l).string () != "false";
      }
      else
      {
        // For the simple case whether this is a test is controlled by the
        // test variable. Also, it feels redundant to specify, say, "test =
        // true" and "test.output = test.out" -- the latter already says this
        // is a test.
        //

        // Use lookup depths to figure out who "overrides" whom.
        //
        auto p (t.find ("test"));

        if (p.first && cast<path> (p.first).string () != "false")
          md.test = true;
        else
        {
          auto test = [&t, &p] (const char* n)
          {
            return t.find (n).second < p.second;
          };

          md.test =
            test ("test.input")     ||
            test ("test.output")    ||
            test ("test.roundtrip") ||
            test ("test.options")   ||
            test ("test.arguments");
        }
      }

      match_result mr (t);

      // If this target is testable and this is the update pre-operation, then
      // all we really need to do is say we are not a match and the standard
      // matching machinery will (hopefully) find the rule to update this
      // target.
      //
      if (md.test && a.operation () == update_id)
      {
        // And this is exactly what we do for the testscript case.
        //
        if (md.script)
          return nullptr;
        else
          // For the simple case there is one thing that compilates this
          // simple approach: test input/output. While normally they will be
          // existing (in src_base) files, they could also be auto-generated.
          // In fact, they could only be needed for testing, which means the
          // normall update won't even know about them (nor clean, for that
          // matter; this is why we need cleantest).
          //
          // @@ Maybe we should just say if input/output are generated, then
          //    they must be explicitly listed as prerequisites? Then no need
          //    for cleantest but they will be updated even when not needed.
          //
          // To make generated input/output work we will have to cause their
          // update ourselves. In other words, we may have to do some actual
          // work for (update, test), and not simply "guide" (update, 0) as to
          // which targets need updating. For how exactly we are going to do
          // it, see apply() below.
          //
          // At this stage we need to change the recipe action to (update, 0)
          // (i.e., "unconditional update").
          //
          mr.recipe_action = action (a.meta_operation (), update_id);
      }

      // Note that we match even if this target is not testable so that we
      // can ignore it (see apply()).
      //
      t.data (md); // Save the data in the target's auxilary storage.
      return mr;
    }

    recipe rule::
    apply (action a, target& t, const match_result&) const
    {
      tracer trace ("test::rule::apply");

      match_data md (move (t.data<match_data> ()));
      t.clear_data (); // In case delegated-to rule also uses aux storage.

      if (!md.test)
        return noop_recipe;

      // If we are here, then the target is testable.
      //
      if (md.script)
      {
        // If we are here, then the action is (perform, test, 0).
        //
        // Collect all the testscript targets in prerequisite_targets.
        //
        for (prerequisite_member p: group_prerequisite_members (a, t))
        {
          if (p.is_a<testscript> ())
            t.prerequisite_targets.push_back (&p.search ());
        }

        return &perform_script;
      }
      else
      {
        // If we are here, then the action is either
        //   a. (perform, test, 0) or
        //   b. (*, update, 0)
        //
        // In both cases, the next step is to see if we have test.{input,
        // output,roundtrip}.
        //

        // We should have either arguments or input/roundtrip. Again, use
        // lookup depth to figure out who takes precedence.
        //
        auto ip (t.find ("test.input"));
        auto op (t.find ("test.output"));
        auto rp (t.find ("test.roundtrip"));
        auto ap (t.find ("test.arguments"));

        auto test = [&t] (pair<lookup, size_t>& x, const char* xn,
                          pair<lookup, size_t>& y, const char* yn)
        {
          if (x.first && y.first)
          {
            if (x.second == y.second)
              fail << "both " << xn << " and " << yn << " specified for "
                   << "target " << t;

            (x.second < y.second ? y : x) = make_pair (lookup (), size_t (~0));
          }
        };

        test (ip, "test.input",     ap, "test.arguments");
        test (rp, "test.roundtrip", ap, "test.arguments");
        test (ip, "test.input",     rp, "test.roundtrip");
        test (op, "test.output",    rp, "test.roundtrip");

        const name* in;
        const name* on;

        // Reduce the roundtrip case to input/output.
        //
        if (rp.first)
        {
          in = on = &cast<name> (rp.first);
        }
        else
        {
          in = ip.first ? &cast<name> (ip.first) : nullptr;
          on = op.first ? &cast<name> (op.first) : nullptr;
        }

        // Resolve them to targets, which normally would be existing files
        // but could also be targets that need updating.
        //
        scope& bs (t.base_scope ());

        // @@ OUT: what if this is a @-qualified pair or names?
        //
        target* it (in != nullptr ? &search (*in, bs) : nullptr);
        target* ot (on != nullptr
                    ? in == on ? it : &search (*on, bs)
                    : nullptr);

        if (a.operation () == update_id)
        {
          // First see if input/output are existing, up-to-date files. This
          // is a common case optimization.
          //
          if (it != nullptr)
          {
            build2::match (a, *it);

            if (it->state () == target_state::unchanged)
            {
              unmatch (a, *it);
              it = nullptr;
            }
          }

          if (ot != nullptr)
          {
            if (in != on)
            {
              build2::match (a, *ot);

              if (ot->state () == target_state::unchanged)
              {
                unmatch (a, *ot);
                ot = nullptr;
              }
            }
            else
              ot = it;
          }

          // Find the "real" update rule, that is, the rule that would have
          // been found if we signalled that we do not match from match()
          // above.
          //
          recipe d (match_delegate (a, t).first);

          // If we have no input/output that needs updating, then simply
          // redirect to it.
          //
          if (it == nullptr && ot == nullptr)
            return d;

          // Ok, time to handle the worst case scenario: we need to cause
          // update of input/output targets and also delegate to the real
          // update.
          //
          return [it, ot, dr = move (d)] (action a, target& t) -> target_state
          {
            // Do the general update first.
            //
            target_state r (execute_delegate (dr, a, t));

            if (it != nullptr)
              r |= execute (a, *it);

            if (ot != nullptr)
              r |= execute (a, *ot);

            return r;
          };
        }
        else
        {
          // Cache the targets in our prerequsite targets lists where they can
          // be found by perform_test(). If we have either or both, then the
          // first entry is input and the second -- output (either can be
          // NULL).
          //
          if (it != nullptr || ot != nullptr)
          {
            auto& pts (t.prerequisite_targets);
            pts.resize (2, nullptr);
            pts[0] = it;
            pts[1] = ot;
          }

          return &perform_test;
        }
      }
    }

    target_state rule::
    perform_script (action, target& t)
    {
      using namespace script;
      using script::script;

      for (target* pt: t.prerequisite_targets)
      {
        testscript& st (*pt->is_a<testscript> ());
        const path& sp (st.path ());
        assert (!sp.empty ()); // Should have been assigned by update.

        text << "test " << t << " with " << st;

        try
        {
          script s (t, st);
          concurrent_runner r;

          ifdstream ifs (sp);
          parser p;
          p.parse (ifs, sp, s, r);
        }
        catch (const io_error& e)
        {
          fail << "unable to read testscript " << sp << ": " << e.what ();
        }
      }

      return target_state::changed;
    }

    // The format of args shall be:
    //
    // name1 arg arg ... nullptr
    // name2 arg arg ... nullptr
    // ...
    // nameN arg arg ... nullptr nullptr
    //
    static bool
    run_test (target& t,
              diag_record& dr,
              char const** args,
              process* prev = nullptr)
    {
      // Find the next process, if any.
      //
      char const** next (args);
      for (next++; *next != nullptr; next++) ;
      next++;

      // Redirect stdout to a pipe unless we are last, in which
      // case redirect it to stderr.
      //
      int out (*next == nullptr ? 2 : -1);
      bool pr, wr;

      try
      {
        if (prev == nullptr)
        {
          // First process.
          //
          process p (args, 0, out);
          pr = *next == nullptr || run_test (t, dr, next, &p);
          wr = p.wait ();
        }
        else
        {
          // Next process.
          //
          process p (args, *prev, out);
          pr = *next == nullptr || run_test (t, dr, next, &p);
          wr = p.wait ();
        }
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }

      if (!wr)
      {
        if (pr) // First failure?
          dr << fail << "test " << t << " failed"; // Multi test: test 1.

        dr << error << "non-zero exit status: ";
        print_process (dr, args);
      }

      return pr && wr;
    }

    target_state rule::
    perform_test (action, target& t)
    {
      // @@ Would be nice to print what signal/core was dumped.
      //
      // @@ Doesn't have to be a file target if we have test.cmd (or
      //    just use test which is now path).
      //

      file& ft (static_cast<file&> (t));
      assert (!ft.path ().empty ()); // Should have been assigned by update.

      process_path fpp (run_search (ft.path (), true));
      cstrings args {fpp.recall_string ()};

      // Do we have options?
      //
      if (auto l = t["test.options"])
        append_options (args, cast<strings> (l));

      // Do we have input?
      //
      auto& pts (t.prerequisite_targets);
      if (pts.size () != 0 && pts[0] != nullptr)
      {
        file& it (static_cast<file&> (*pts[0]));
        assert (!it.path ().empty ()); // Should have been assigned by update.
        args.push_back (it.path ().string ().c_str ());
      }
      // Maybe arguments then?
      //
      else
      {
        if (auto l = t["test.arguments"])
          append_options (args, cast<strings> (l));
      }

      args.push_back (nullptr);

      // Do we have output?
      //
      path dp ("diff");
      process_path dpp;
      if (pts.size () != 0 && pts[1] != nullptr)
      {
        file& ot (static_cast<file&> (*pts[1]));
        assert (!ot.path ().empty ()); // Should have been assigned by update.

        dpp = run_search (dp, true);

        args.push_back (dpp.recall_string ());
        args.push_back ("--strip-trailing-cr"); //@@ TMP: see module.cxx
        args.push_back ("-u");
        args.push_back (ot.path ().string ().c_str ());
        args.push_back ("-");
        args.push_back (nullptr);
      }

      args.push_back (nullptr); // Second.

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "test " << t;

      {
        diag_record dr;

        if (!run_test (t, dr, args.data ()))
        {
          dr << info << "test command line: ";
          print_process (dr, args);
        }
      }

      return target_state::changed;
    }
  }
}
