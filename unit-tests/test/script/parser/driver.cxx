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
      class print_runner: public runner
      {
      public:
        virtual void
        run (const command& t) override
        {
          // Here we assume we are running serially.
          //
          cout << t << endl;
        }
      };

      int
      main ()
      {
        tracer trace ("main");

        init (1);           // Default verbosity.
        reset (strings ()); // No command line variables.

        try
        {
          path name ("testscript");
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
          st.path (path ("testscript"));

          // Parse and run.
          //
          script s (tt, st);
          print_runner r;

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
main ()
{
  return build2::test::script::main ();
}
