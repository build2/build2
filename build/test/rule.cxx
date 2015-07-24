// file      : build/test/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/test/rule>

#include <butl/process>
#include <butl/fdstream>

#include <build/scope>
#include <build/target>
#include <build/algorithm>
#include <build/diagnostics>

using namespace std;
using namespace butl;

namespace build
{
  namespace test
  {
    match_result rule::
    match (action a, target& t, const std::string&) const
    {
      // First determine if this is a test. This is controlled by
      // the test target variable and text.<tt> scope variables.
      // Also, it feels redundant to specify, say, "test = true"
      // and "test.output = test.out" -- the latter already says
      // this is a test. So take care of that as well.
      //
      bool r (false);
      value_proxy v;

      for (auto p (t.vars.find_namespace ("test"));
           p.first != p.second;
           ++p.first)
      {
        const variable& var (p.first->first);
        value_ptr& val (p.first->second);

        // If we have test, then always use that.
        //
        if (var.name == "test")
        {
          v.rebind (value_proxy (val, t));
          break;
        }

        // Otherwise check for variables that would indicate this
        // is a test.
        //
        if (var.name == "test.input"     ||
            var.name == "test.ouput"     ||
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
        // See if the is a scope variable.
        //
        if (!v)
          v.rebind (t.base_scope ()[string("test.") + t.type ().name]);

        r = v && v.as<bool> ();

      }

      if (!r)
        return match_result (t, false); // "Not a test" result.

      // If this is the update pre-operation, make someone else do
      // the job.
      //
      if (a.operation () != test_id)
        return nullptr;

      return match_result (t, true);
    }

    recipe rule::
    apply (action a, target& t, const match_result& mr) const
    {
      tracer trace ("test::rule::apply");

      if (!mr.value) // Not a test.
        return noop_recipe;

      // Don't do anything for other meta-operations.
      //
      if (a != action (perform_id, test_id))
        return noop_recipe;

      // See if we have test.{input,output,roundtrip}. First check the
      // target-specific vars since they override any scope ones.
      //
      auto iv (t.vars["test.input"]);
      auto ov (t.vars["test.output"]);
      auto rv (t.vars["test.roundtrip"]);

      // Can either be input or arguments.
      //
      auto av (t.vars["test.arguments"]);

      if (av)
      {
        if (iv)
          fail << "both test.input and test.arguments specified for "
               << "target " << t;

        if (rv)
          fail << "both test.roundtrip and test.arguments specified for "
               << "target " << t;
      }

      scope& bs (t.base_scope ());

      if (!iv && !ov && !rv)
      {
        string n ("test.");
        n += t.type ().name;

        const variable& in (variable_pool.find (n + ".input"));
        const variable& on (variable_pool.find (n + ".output"));
        const variable& rn (variable_pool.find (n + ".roundtrip"));

        // We should only keep value(s) that were specified together
        // in the innermost scope.
        //
        for (scope* s (&bs); s != nullptr; s = s->parent_scope ())
        {
          ov.rebind (s->vars[on]);

          if (!av) // Not overriden at target level by test.arguments?
          {
            iv.rebind (s->vars[in]);
            rv.rebind (s->vars[rn]);
          }

          if (iv || ov || rv)
            break;
        }
      }

      const name* i;
      const name* o;

      // Reduce the roundtrip case to input/output.
      //
      if (rv)
      {
        if (iv || ov)
          fail << "both test.roundtrip and test.input/output specified "
               << "for target " << t;

        i = o = rv.as<const name*> ();
      }
      else
      {
        i = iv ? iv.as<const name*> () : nullptr;
        o = ov ? ov.as<const name*> () : nullptr;
      }

      // Resolve them to targets (normally just files) and cache in
      // our prerequsite targets lists where they can be found by
      // perform_test(). If we have either or both, then the first
      // entry is input and the second -- output (either can be NULL).
      //
      auto& pts (t.prerequisite_targets);

      if (i != nullptr || o != nullptr)
        pts.resize (2, nullptr);

      //@@ We should match() them, but for update, not test.
      //@@ If not doing this for some reason, need to then verify
      //   path was assigned (i.e., matched an existing file).
      //
      if (i != nullptr)
        pts[0] = &search (*i, bs);

      if (o != nullptr)
        pts[1] = i == o ? pts[0] : &search (*o, bs);

      return &perform_test;
    }

    static void
    add_arguments (cstrings& args, target& t, const char* n)
    {
      string var ("test.");
      var += n;

      auto v (t.vars[var]);

      if (!v)
      {
        var.resize (5);
        var += t.type ().name;
        var += '.';
        var += n;
        v.rebind (t.base_scope ()[var]);
      }

      if (v)
      {
        for (const name& n: v.as<const list_value&> ())
        {
          if (n.simple ())
            args.push_back (n.value.c_str ());
          else if (n.directory ())
            args.push_back (n.dir.string ().c_str ());
          else
            fail << "expected argument instead of " << n <<
              info << "in variable " << var;
        }
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
        assert (!it.path ().empty ()); // Should have been assigned.
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
        assert (!ot.path ().empty ()); // Should have been assigned.

        args.push_back ("diff");
        args.push_back ("-u");
        args.push_back (ot.path ().string ().c_str ());
        args.push_back ("-");
        args.push_back (nullptr);
      }

      args.push_back (nullptr); // Second.

      if (verb)
        print_process (args);
      else
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
