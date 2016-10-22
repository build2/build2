// file      : unit-tests/test/script/parser/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <build2/types>
#include <build2/utility>

#include <build2/target>
#include <build2/context>

#include <build2/test/target>

#include <build2/test/script/token>
#include <build2/test/script/parser>
#include <build2/test/script/runner>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      // Here we assume we are running serially.
      //
      class print_runner: public runner
      {
      public:
        print_runner (bool scope): scope_ (scope) {}

        virtual void
        enter (scope&, const location&) override
        {
          if (scope_)
          {
            cout << ind_ << "{" << endl;
            ind_ += "  ";
          }
        }

        virtual void
        run (scope&, const command& t, size_t, const location&) override
        {
          cout << ind_ << t << endl;
        }

        virtual void
        leave (scope&, const location&) override
        {
          if (scope_)
          {
            ind_.resize (ind_.size () - 2);
            cout << ind_ << "}" << endl;
          }
        }

      private:
        bool scope_;
        string ind_;
      };

      // Usage: argv[0] [-s] [<testscript-name>]
      //
      int
      main (int argc, char* argv[])
      {
        tracer trace ("main");

        init (1);           // Default verbosity.
        reset (strings ()); // No command line variables.

        bool scope (false);
        path name;

        for (int i (1); i != argc; ++i)
        {
          string a (argv[i]);

          if (a == "-s")
            scope = true;
          else
          {
            name = path (move (a));
            break;
          }
        }

        if (name.empty ())
          name = path ("testscript");

        try
        {
          cin.exceptions (istream::failbit | istream::badbit);

          // Enter mock targets. Use fixed names and paths so that we can use
          // them in expected results. Strictly speaking target paths should
          // be absolute. However, the testscript implementation doesn't
          // really care.
          //
          file& tt (
            targets.insert<file> (work,
                                  dir_path (),
                                  "driver",
                                  &extension_pool.find (""),
                                  trace));

          testscript& st (
            targets.insert<testscript> (work,
                                        dir_path (),
                                        "testscript",
                                        &extension_pool.find (""),
                                        trace));

          tt.path (path ("driver"));
          st.path (name);

          // Parse and run.
          //
          script s (tt, st);
          print_runner r (scope);

          parser p;
          p.pre_parse (cin, name, s);
          p.parse (name, s, r);
        }
        catch (const failed&)
        {
          return 1;
        }

        return 0;
      }
    }
  }
}

int
main (int argc, char* argv[])
{
  return build2::test::script::main (argc, argv);
}
