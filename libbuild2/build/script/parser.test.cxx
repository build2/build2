// file      : libbuild2/build/script/parser.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/scheduler.hxx>

#include <libbuild2/build/script/script.hxx> // line
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
      // argv[0] -p
      //
      // In the first form read the script from stdin and trace the script
      // execution to stdout using the custom print runner.
      //
      // In the second form read the script from stdin, parse it and dump the
      // resulting lines to stdout.
      //
      // In the third form read the script from stdin, parse it and print
      // line tokens quoting information to stdout.
      //
      // -l
      //    Print the script line number for each executed expression.
      //
      // -d
      //    Dump the parsed script to sdout.
      //
      // -p
      //    Print the parsed script tokens quoting information to sdout. If a
      //    token is quoted follow its representation with its quoting
      //    information in the [<quoting>/<completeness>] form, where:
      //
      //    <quoting>      := 'S' | 'D' | 'M'
      //    <completeness> := 'C' | 'P'
      //
      int
      main (int argc, char* argv[])
      {
        tracer trace ("main");

        enum class mode
        {
          run,
          dump,
          print
        } m (mode::run);

        bool print_line (false);

        for (int i (1); i != argc; ++i)
        {
          string a (argv[i]);

          if (a == "-l")
            print_line = true;
          else if (a == "-d")
            m = mode::dump;
          else if (a == "-p")
            m = mode::print;
          else
            assert (false);
        }

        assert (m == mode::run || !print_line);

        // Fake build system driver, default verbosity.
        //
        init_diag (1);
        init (nullptr, argv[0]);

        // Serial execution.
        //
        scheduler sched (1);
        global_mutexes mutexes (1);
        context ctx (sched, mutexes);

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
          parser p (ctx);
          path_name nm ("buildfile");
          script s (p.pre_parse (cin, nm, 11 /* line */, nullopt));

          switch (m)
          {
          case mode::run:
            {
              environment e (perform_update_id, tt, false /* temp_dir */);
              print_runner r (print_line);
              p.execute (ctx.global_scope, ctx.global_scope, e, s, r);
              break;
            }
          case mode::dump:
            {
              dump (cout, "", s.lines);
              break;
            }
          case mode::print:
            {
              for (const line& l: s.lines)
              {
                for (const replay_token& rt: l.tokens)
                {
                  if (&rt != &l.tokens[0])
                    cout << ' ';

                  const token& t (rt.token);
                  cout << t;

                  char q ('\0');
                  switch (t.qtype)
                  {
                  case quote_type::single:   q = 'S'; break;
                  case quote_type::double_:  q = 'D'; break;
                  case quote_type::mixed:    q = 'M'; break;
                  case quote_type::unquoted:          break;
                  }

                  if (q != '\0')
                    cout << " [" << q << (t.qcomp ? "/C" : "/P") << ']';
                }
              }

              cout << endl;
            }
          }
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
