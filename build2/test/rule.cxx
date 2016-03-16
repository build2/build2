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
      // First determine if this is a test. This is controlled by
      // the test target variable and text.<tt> scope variables.
      // Also, it feels redundant to specify, say, "test = true"
      // and "test.output = test.out" -- the latter already says
      // this is a test. So take care of that as well.
      //
      bool r (false);
      lookup<const value> l;

      // @@ This logic doesn't take into account target type/pattern-
      // specific variables.
      //
      // @@ Perhaps a find_any(<list-of-vars>)?
      //
      for (auto p (t.vars.find_namespace ("test"));
           p.first != p.second;
           ++p.first)
      {
        const variable& var (p.first->first);
        const value& val (p.first->second);

        // If we have test, then always use that.
        //
        if (var.name == "test")
        {
          l = lookup<const value> (val, t);
          break;
        }

        // Otherwise check for variables that would indicate this
        // is a test.
        //
        if (var.name == "test.input"     ||
            var.name == "test.output"    ||
            var.name == "test.roundtrip" ||
            var.name == "test.options"   ||
            var.name == "test.arguments")
        {
          r = true;
          break;
        }
      }

      if (!r)
      {
        // See if there is a scope variable.
        //
        if (!l.defined ())
          l = t.base_scope ()[
            var_pool.find<bool> (string("test.") + t.type ().name)];

        r = l && as<bool> (*l);
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

      // First check the target-specific vars since they override any
      // scope ones.
      //
      auto il (t.vars["test.input"]);
      auto ol (t.vars["test.output"]);
      auto rl (t.vars["test.roundtrip"]);
      auto al (t.vars["test.arguments"]); // Should be input or arguments.

      if (al)
      {
        if (il)
          fail << "both test.input and test.arguments specified for "
               << "target " << t;

        if (rl)
          fail << "both test.roundtrip and test.arguments specified for "
               << "target " << t;
      }

      scope& bs (t.base_scope ());

      if (!il && !ol && !rl)
      {
        string n ("test.");
        n += t.type ().name;

        const variable& in (var_pool.find<name> (n + ".input"));
        const variable& on (var_pool.find<name> (n + ".output"));
        const variable& rn (var_pool.find<name> (n + ".roundtrip"));

        // We should only keep value(s) that were specified together
        // in the innermost scope.
        //
        // @@ Shouldn't we stop at project root?
        //
        for (scope* s (&bs); s != nullptr; s = s->parent_scope ())
        {
          ol = s->vars[on];

          if (!al) // Not overriden at target level by test.arguments?
          {
            il = s->vars[in];
            rl = s->vars[rn];
          }

          if (il || ol || rl)
            break;
        }
      }

      const name* in;
      const name* on;

      // Reduce the roundtrip case to input/output.
      //
      if (rl)
      {
        if (il || ol)
          fail << "both test.roundtrip and test.input/output specified "
               << "for target " << t;

        in = on = &as<name> (*rl);
      }
      else
      {
        in = il ? &as<name> (*il) : nullptr;
        on = ol ? &as<name> (*ol) : nullptr;
      }

      // Resolve them to targets, which normally would be existing files
      // but could also be targets that need updating.
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

    static void
    add_arguments (cstrings& args, const target& t, const char* n)
    {
      string var ("test.");
      var += n;

      auto l (t.vars[var]);

      if (!l)
      {
        var.resize (5);
        var += t.type ().name;
        var += '.';
        var += n;
        l = t.base_scope ()[var_pool.find<strings> (var)];
      }

      if (l)
        append_options (args, as<strings> (*l));
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

      cstrings args {ft.path ().string ().c_str ()};

      // Do we have options?
      //
      add_arguments (args, t, "options");

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
        add_arguments (args, t, "arguments");

      args.push_back (nullptr);

      // Do we have output?
      //
      if (pts.size () != 0 && pts[1] != nullptr)
      {
        file& ot (static_cast<file&> (*pts[1]));
        assert (!ot.path ().empty ()); // Should have been assigned by update.

        args.push_back ("diff");
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
