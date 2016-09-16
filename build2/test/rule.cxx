// file      : build2/test/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/rule>

#include <build2/scope>
#include <build2/target>
#include <build2/algorithm>
#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    match_result rule::
    match (action a, target& t, const string&) const
    {
      // First determine if this is a test. This is controlled by the test
      // variable. Also, it feels redundant to specify, say, "test = true" and
      // "test.output = test.out" -- the latter already says this is a test.
      //
      bool r (false);
      {
        // Use lookup depths to figure out who "overrides" whom.
        //
        auto p (t.find ("test"));

        if (p.first && cast<bool> (p.first))
          r = true;
        else
        {
          auto test = [&t, &p] (const char* n)
          {
            return t.find (n).second < p.second;
          };

          r = test ("test.input")   ||
            test ("test.output")    ||
            test ("test.roundtrip") ||
            test ("test.options")   ||
            test ("test.arguments");
        }
      }

      // If this is the update pre-operation, then all we really need to
      // do is say we are not a match and the standard matching machinery
      // will (hopefully) find the rule to update this target.
      //
      // There is one thing that compilates this simple approach: test
      // input/output. While normally they will be existing (in src_base)
      // files, they could also be auto-generated. In fact, they could
      // only be needed for testing, which means the normall update won't
      // even know about them (nor clean, for that matter; this is why we
      // need cleantest).
      //
      // To make generated input/output work we will have to cause their
      // update ourselves. I other words, we may have to do some actual
      // work for (update, test), and not simply "guide" (update, 0) as
      // to which targets need updating. For how exactly we are going to
      // do it, see apply() below.
      //
      match_result mr (t, r);

      // If this is the update pre-operation, change the recipe action
      // to (update, 0) (i.e., "unconditional update").
      //
      if (r && a.operation () == update_id)
        mr.recipe_action = action (a.meta_operation (), update_id);

      return mr;
    }

    recipe rule::
    apply (action a, target& t, const match_result& mr) const
    {
      tracer trace ("test::rule::apply");

      if (!mr.bvalue) // Not a test.
        return noop_recipe;

      // Ok, if we are here, then this means:
      //
      // 1. This target is a test.
      // 2. The action is either
      //    a. (perform, test, 0) or
      //    b. (*, update, install)
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
      target* ot (on != nullptr ? in == on ? it : &search (*on, bs) : nullptr);

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


        // Find the "real" update rule, that is, the rule that would
        // have been found if we signalled that we do not match from
        // match() above.
        //
        recipe d (match_delegate (a, t).first);

        // If we have no input/output that needs updating, then simply
        // redirect to it.
        //
        if (it == nullptr && ot == nullptr)
          return d;

        // Ok, time to handle the worst case scenario: we need to
        // cause update of input/output targets and also delegate
        // to the real update.
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
        // Cache the targets in our prerequsite targets lists where they
        // can be found by perform_test(). If we have either or both,
        // then the first entry is input and the second -- output (either
        // can be NULL).
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
      // @@ Doesn't have to be a file target if we have test.cmd.
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
