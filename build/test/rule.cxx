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
      // First determine if this is a test.
      //
      auto v (t.vars["test"]);

      if (!v)
        v.rebind (t.base_scope ()[string("test.") + t.type ().name]);

      if (!v || !v.as<bool> ())
        return match_result (t, false); // "Not a test" result.

      // If this is the update pre-operation, make someone else do
      // the job.
      //
      if (a.operation () != test_id)
        return nullptr;

      return match_result (t, true);
    }

    recipe rule::
    apply (action a, target&, const match_result& mr) const
    {
      if (!mr.value) // Not a test.
        return noop_recipe;

      return a == action (perform_id, test_id)
        ? &perform_test
        : noop_recipe; // Don't do anything for other meta-operations.
    }

    // The format of args shall be:
    //
    // name1 arg arg ... nullptr
    // name2 arg arg ... nullptr
    // ...
    // nameN arg arg ... nullptr nullptr
    //
    static bool
    pipe_process (char const** args, process* prev = nullptr)
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

      if (prev == nullptr)
      {
        // First process.
        //
        process p (args, 0, out);
        pr = *next == nullptr || pipe_process (next, &p);
        wr = p.wait ();
      }
      else
      {
        // Next process.
        //
        process p (args, *prev, out);
        pr = *next == nullptr || pipe_process (next, &p);
        wr = p.wait ();
      }

      if (!wr)
      {
        // @@ Needs to go into the same diag record.
        //
        error << "non-zero exit status from:";
        print_process (args);
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

      cstrings args {ft.path ().string ().c_str (), nullptr};

      args.push_back ("diff");
      args.push_back ("-u");
      args.push_back ("test.std");
      args.push_back ("-");
      args.push_back (nullptr);

      args.push_back (nullptr); // Second.

      if (verb)
        print_process (args);
      else
        text << "test " << t;

      try
      {
        if (!pipe_process (args.data ()))
        {
          //@@ Need to use the same diag record.
          //
          error << "failed test:";
          print_process (args);
          throw failed ();
        }

        return target_state::changed;
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }
  }
}
