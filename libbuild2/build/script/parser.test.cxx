// file      : libbuild2/build/script/parser.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/scheduler.hxx>

#include <libbuild2/build/script/parser.hxx>
#include <libbuild2/build/script/runner.hxx>

using namespace std;

namespace build2
{
  namespace build
  {
    namespace script
    {
      class print_runner: public runner
      {
      public:
        print_runner (bool line): line_ (line) {}

        virtual void
        enter (environment&, const location&) override {}

        virtual void
        run (environment&,
             const command_expr& e,
             size_t i,
             const location&) override
        {
          cout << e;

          if (line_)
            cout << " # " << i;

          cout << endl;
        }

        virtual bool
        run_if (environment&,
                const command_expr& e,
                size_t i,
                const location&) override
        {
          cout << "? " << e;

          if (line_)
            cout << " # " << i;

          cout << endl;

          return e.back ().pipe.back ().program.string () == "true";
        }

        virtual void
        leave (environment&, const location&) override {}

      private:
        bool line_;
      };

      // Usages:
      //
      // argv[0] [-l]
      // argv[0] -d
      //
      int
      main (int argc, char* argv[])
      {
        tracer trace ("main");

        // Fake build system driver, default verbosity.
        //
        init_diag (1);
        init (nullptr, argv[0]);

        // Serial execution.
        //
        scheduler sched (1);
        global_mutexes mutexes (1);
        context ctx (sched, mutexes);

        bool line (false);
        bool dump (false);

        for (int i (1); i != argc; ++i)
        {
          string a (argv[i]);

          if (a == "-l")
            line = true;
          else if (a == "-d")
            dump = true;
          else
            assert (false);
        }

        assert (!dump || !line);

        try
        {
          cin.exceptions (istream::failbit | istream::badbit);

          // Enter mock target. Use fixed name and path so that we can use
          // them in expected results. Strictly speaking target path should
          // be absolute. However, the buildscript implementation doesn't
          // really care.
          //
          file& tt (
            ctx.targets.insert<file> (work,
                                      dir_path (),
                                      "driver",
                                      string (),
                                      trace));

          tt.path (path ("driver"));

          // Parse and run.
          //
          path_name nm ("buildfile");

          script s;
          parser p (ctx);
          p.pre_parse (cin, nm, 11 /* line */, s);

          if (!dump)
          {
            environment e (s, tt);
            print_runner r (line);
            p.execute (e, r);
          }
          else
            build2::script::dump (cout, "", s.lines);
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
  return build2::build::script::main (argc, argv);
}
