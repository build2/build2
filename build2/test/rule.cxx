// file      : build2/test/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/rule>

#include <build2/scope>
#include <build2/target>
#include <build2/algorithm>
#include <build2/filesystem>
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
      bool pass; // Pass-through to prerequsites (for alias only).
      bool test;

      bool script;
    };

    static_assert (sizeof (match_data) <= target::data_size,
                   "insufficient space");

    match_result rule_common::
    match (slock& ml, action a, target& t, const string&) const
    {
      // The (admittedly twisted) logic of this rule tries to achieve the
      // following: If the target is testable, then we want both first update
      // it and then test it. Otherwise, we want to ignore it for both
      // operations. To achieve this the rule will be called to match during
      // both operations. For update, if the target is not testable, then the
      // rule matches with a noop recipe. If the target is testable, then the
      // rule also matches but delegates to the real update rule. In this case
      // (and this is tricky) the rule also changes the operation to
      // unconditional update to make sure it doesn't match any prerequisites
      // (which, if not testable, it will noop).
      //
      // And to add a bit more complexity, we want to handle aliases slightly
      // differently: we may not want to ignore their prerequisites if the
      // alias is not testable since their prerequisites could be.

      match_data md {t.is_a<alias> () && pass (t), false, false};

      if (test (t))
      {
        // We have two very different cases: testscript and simple test (plus
        // it may not be a testable target at all). So as the first step
        // determine which case this is.
        //
        // If we have any prerequisites of the test{} type, then this is the
        // testscript case.
        //
        for (prerequisite_member p: group_prerequisite_members (ml, a, t))
        {
          if (p.is_a<testscript> ())
          {
            md.script = true;

            // We treat this target as testable unless the test variable is
            // explicitly set to false.
            //
            const name* n (cast_null<name> (t["test"]));
            md.test = n == nullptr || !n->simple () || n->value != "false";
            break;
          }
        }

        // If this is not a script, then determine if it is a simple test.
        // Ignore aliases and testscripts files themselves at the outset.
        //
        if (!md.script && !t.is_a<alias> () && !t.is_a<testscript> ())
        {
          // For the simple case whether this is a test is controlled by the
          // test variable. Also, it feels redundant to specify, say, "test =
          // true" and "test.output = test.out" -- the latter already says this
          // is a test.
          //

          // Use lookup depths to figure out who "overrides" whom.
          //
          auto p (t.find (var_pool["test"]));
          const name* n (cast_null<name> (p.first));

          // Note that test can be set to an "override" target.
          //
          if (n != nullptr && (!n->simple () || n->value != "false"))
            md.test = true;
          else
          {
            auto test = [&t, &p] (const char* var)
            {
              return t.find (var_pool[var]).second < p.second;
            };

            md.test =
              test ("test.input")     ||
              test ("test.output")    ||
              test ("test.roundtrip") ||
              test ("test.options")   ||
              test ("test.arguments");
          }
        }
      }

      match_result mr (true);

      // Theoretically if this target is testable and this is the update
      // pre-operation, then all we need to do is say we are not a match and
      // the standard matching machinery will find the rule to update this
      // target. The problem with this approach is that the matching will
      // still happen for "update for test" which means this rule may still
      // match prerequisites (e.g., non-existent files) which we don't want.
      //
      // Also, for the simple case there is one more complication: test
      // input/output. While normally they will be existing (in src_base)
      // files, they could also be auto-generated. In fact, they could only be
      // needed for testing, which means the normall update won't even know
      // about them (nor clean, for that matter; this is why we need
      // cleantest).
      //
      // @@ Maybe we should just say if input/output are generated, then they
      //    must be explicitly listed as prerequisites? Then no need for
      //    cleantest but they will be updated even when not needed.
      //
      // To make generated input/output work we will have to cause their
      // update ourselves. In other words, we may have to do some actual work
      // for (update, test), and not simply "guide" (update, 0) as to which
      // targets need updating. For how exactly we are going to do it, see
      // apply() below.
      //
      // Change the recipe action to (update, 0) (i.e., "unconditional
      // update") to make sure we won't match any prerequisites.
      //
      if (a.operation () == update_id && (md.pass || md.test))
        mr.recipe_action = action (a.meta_operation (), update_id);

      // Note that we match even if this target is not testable so that we can
      // ignore it (see apply()).
      //
      t.data (md); // Save the data in the target's auxilary storage.
      return mr;
    }

    recipe alias_rule::
    apply (slock& ml, action a, target& t) const
    {
      match_data md (move (t.data<match_data> ()));
      t.clear_data (); // In case delegated-to rule also uses aux storage.

      // We can only test an alias via a testscript, not a simple test.
      //
      assert (!md.test || md.script);

      if (!md.pass && !md.test)
        return noop_recipe;

      // If this is the update pre-operation then simply redirect to the
      // standard alias rule.
      //
      if (a.operation () == update_id)
        return match_delegate (ml, a, t, *this).first;

      // For the test operation we have to implement our own search and match
      // because we need to ignore prerequisites that are outside of our
      // project. They can be from projects that don't use the test module
      // (and thus won't have a suitable rule). Or they can be from no project
      // at all (e.g., installed). Also, generally, not testing stuff that's
      // not ours seems right. Note that we still want to make sure they are
      // up to date (via the above delegate) since our tests might use them.
      //
      search_and_match_prerequisites (ml, a, t, t.root_scope ());

      // If not a test then also redirect to the alias rule.
      //
      return md.test
        ? [this] (action a, const target& t) {return perform_test (a, t);}
        : default_recipe;
    }

    recipe rule::
    apply (slock& ml, action a, target& t) const
    {
      tracer trace ("test::rule::apply");

      match_data md (move (t.data<match_data> ()));
      t.clear_data (); // In case delegated-to rule also uses aux storage.

      if (!md.test)
        return noop_recipe;

      // If we are here, then the target is testable and the action is either
      //   a. (perform, test, 0) or
      //   b. (*, update, 0)
      //
      if (md.script)
      {
        if (a.operation () == update_id)
          return match_delegate (ml, a, t, *this).first;

        // Collect all the testscript targets in prerequisite_targets.
        //
        for (prerequisite_member p: group_prerequisite_members (ml, a, t))
        {
          if (p.is_a<testscript> ())
            t.prerequisite_targets.push_back (&p.search ());
        }

        return [this] (action a, const target& t)
        {
          return perform_script (a, t);
        };
      }
      else
      {
        // In both cases, the next step is to see if we have test.{input,
        // output,roundtrip}.
        //

        // We should have either arguments or input/roundtrip. Again, use
        // lookup depth to figure out who takes precedence.
        //
        auto ip (t.find (var_pool["test.input"]));
        auto op (t.find (var_pool["test.output"]));
        auto rp (t.find (var_pool["test.roundtrip"]));
        auto ap (t.find (var_pool["test.arguments"]));

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
        const scope& bs (t.base_scope ());

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
            build2::match (ml, a, *it);

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
              build2::match (ml, a, *ot);

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
          recipe d (match_delegate (ml, a, t, *this).first);

          // If we have no input/output that needs updating, then simply
          // redirect to it.
          //
          if (it == nullptr && ot == nullptr)
            return d;

          // Ok, time to handle the worst case scenario: we need to cause
          // update of input/output targets and also delegate to the real
          // update.
          //
          return [it, ot, dr = move (d)] (
            action a, const target& t) -> target_state
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

    target_state rule_common::
    perform_script (action, const target& t) const
    {
      // Figure out whether the testscript file is called 'testscript', in
      // which case it should be the only one.
      //
      bool one;
      {
        optional<bool> o;
        for (const target* pt: t.prerequisite_targets)
        {
          // In case we are using the alias rule's list (see above).
          //
          if (const testscript* ts = pt->is_a<testscript> ())
          {
            bool r (ts->name == "testscript");

            if ((r && o) || (!r && o && *o))
              fail << "both 'testscript' and other names specified for " << t;

            o = r;
          }
        }

        assert (o); // We should have a testscript or we wouldn't be here.
        one = *o;
      }

      // Calculate root working directory. It is in the out_base of the target
      // and is called just test for dir{} targets and test-<target-name> for
      // other targets.
      //
      dir_path wd (t.out_dir ());

      if (t.is_a<dir> ())
        wd /= "test";
      else
        wd /= "test-" + t.name;

      // If this is a (potentially) multi-testscript test, then create (and
      // later cleanup) the root directory. If this is just 'testscript', then
      // the root directory is used directly as test's working directory and
      // it's the runner's responsibility to create and clean it up.
      //
      // What should we do if the directory already exists? We used to fail
      // which meant the user had to go and clean things up manually every
      // time a test failed. This turned out to be really annoying. So now we
      // issue a warning and clean it up automatically. The drawbacks of this
      // approach are the potential loss of data from the previous failed test
      // run and the possibility of deleting user-created files.
      //
      if (exists (static_cast<const path&> (wd), false))
        fail << "working directory " << wd << " is a file/symlink";

      if (exists (wd))
      {
        warn << "working directory " << wd << " exists "
             << (empty (wd) ? "" : "and is not empty ") << "at the beginning "
             << "of the test";

        // Remove the directory itself not to confuse the runner which tries
        // to detect when tests stomp on each others feet.
        //
        build2::rmdir_r (wd, true, 2);
      }

      // Delay actually creating the directory in case all the tests are
      // ignored (via config.test).
      //
      bool mk (!one);

      // Run all the testscripts.
      //
      for (const target* pt: t.prerequisite_targets)
      {
        if (const testscript* ts = pt->is_a<testscript> ())
        {
          // If this is just the testscript, then its id path is empty (and
          // it can only be ignored by ignoring the test target, which makes
          // sense since it's the only testscript file).
          //
          if (one || test (t, path (ts->name)))
          {
            if (mk)
            {
              mkdir (wd, 2);
              mk = false;
            }

            if (verb)
            {
              const auto& tt (cast<target_triplet> (t["test.target"]));
              text << "test " << t << " with " << *ts << " on " << tt;
            }

            script::parser p;
            script::script s (t, *ts, wd);
            p.pre_parse (s);

            script::default_runner r (*this);
            p.execute (s, r);
          }
        }
      }

      // Cleanup.
      //
      if (!one && !mk)
      {
        if (!empty (wd))
          fail << "working directory " << wd << " is not empty at the "
               << "end of the test";

        rmdir (wd, 2);
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
    run_test (const target& t,
              diag_record& dr,
              char const** args,
              process* prev = nullptr)
    {
      // Find the next process, if any.
      //
      char const** next (args);
      for (next++; *next != nullptr; next++) ;
      next++;

      // Redirect stdout to a pipe unless we are last.
      //
      int out (*next != nullptr ? -1 : 1);
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
        error << "unable to execute " << args[0] << ": " << e;

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
    perform_test (action, const target& tt)
    {
      // @@ Would be nice to print what signal/core was dumped.
      //

      // See if we have the test executable override.
      //
      path p;
      {
        // Note that the test variable's visibility is target.
        //
        lookup l (tt["test"]);

        // Note that we have similar code for scripted tests.
        //
        const target* t (nullptr);

        if (l.defined ())
        {
          const name* n (cast_null<name> (l));

          if (n == nullptr)
            fail << "invalid test executable override: null value";
          else if (n->empty ())
            fail << "invalid test executable override: empty value";
          else if (n->simple ())
          {
            // Ignore the special 'true' value.
            //
            if (n->value != "true")
              p = path (n->value);
            else
              t = &tt;
          }
          else if (n->directory ())
            fail << "invalid test executable override: '" << *n << "'";
          else
          {
            // Must be a target name.
            //
            // @@ OUT: what if this is a @-qualified pair or names?
            //
            t = search_existing (*n, tt.base_scope ());

            if (t == nullptr)
              fail << "invalid test executable override: unknown target: '"
                   << n << "'";
          }
        }
        else
          // By default we set it to the test target's path.
          //
          t = &tt;

        if (t != nullptr)
        {
          if (auto* pt = t->is_a<path_target> ())
          {
            // Do some sanity checks: the target better be up-to-date with
            // an assigned path.
            //
            p = pt->path ();

            if (p.empty ())
              fail << "target " << *pt << " specified in the test variable "
                   << "is out of date" <<
                info << "consider specifying it as a prerequisite of " << tt;
          }
          else
            fail << "target " << *t << (t != &tt
                                        ? " specified in the test variable "
                                        : " requested to be tested ")
                 << "is not path-based";
        }
      }

      process_path pp (run_search (p, true));
      cstrings args {pp.recall_string ()};

      // Do we have options?
      //
      if (auto l = tt["test.options"])
        append_options (args, cast<strings> (l));

      // Do we have input?
      //
      auto& pts (tt.prerequisite_targets);
      if (pts.size () != 0 && pts[0] != nullptr)
      {
        const file& it (static_cast<const file&> (*pts[0]));
        assert (!it.path ().empty ()); // Should have been assigned by update.
        args.push_back (it.path ().string ().c_str ());
      }
      // Maybe arguments then?
      //
      else
      {
        if (auto l = tt["test.arguments"])
          append_options (args, cast<strings> (l));
      }

      args.push_back (nullptr);

      // Do we have output?
      //
      path dp ("diff");
      process_path dpp;
      if (pts.size () != 0 && pts[1] != nullptr)
      {
        const file& ot (static_cast<const file&> (*pts[1]));
        assert (!ot.path ().empty ()); // Should have been assigned by update.

        dpp = run_search (dp, true);

        args.push_back (dpp.recall_string ());
        args.push_back ("-u");

        // Ignore Windows newline fluff if that's what we are running on.
        //
        if (cast<target_triplet> (tt["test.target"]).class_ == "windows")
          args.push_back ("--strip-trailing-cr");

        args.push_back (ot.path ().string ().c_str ());
        args.push_back ("-");
        args.push_back (nullptr);
      }

      args.push_back (nullptr); // Second.

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "test " << tt;

      diag_record dr;
      if (!run_test (tt, dr, args.data ()))
      {
        dr << info << "test command line: ";
        print_process (dr, args);
        dr << endf; // return
      }

      return target_state::changed;
    }

    target_state alias_rule::
    perform_test (action a, const target& t) const
    {
      // Run the alias recipe first then the test.
      //
      target_state r (execute_prerequisites (a, t));

      // Note that we reuse the prerequisite_targets prepared by the standard
      // search and match.
      //
      return r |= perform_script (a, t);
    }
  }
}
