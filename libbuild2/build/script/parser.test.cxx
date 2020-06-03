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

          return e.back ().pipe.back ().program.recall.string () == "true";
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
      // argv[0] -g [<diag-name>]
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
      // In the forth form read the script from stdin, parse it and print the
      // low-verbosity script diagnostics name or custom low-verbosity
      // diagnostics to stdout. If the script doesn't deduce any of them, then
      // print the diagnostics and exit with non-zero code.
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
      // -g
      //    Dump the low-verbosity script diagnostics name or custom
      //    low-verbosity diagnostics to stdout.
      //
      int
      main (int argc, char* argv[])
      {
        tracer trace ("main");

        enum class mode
        {
          run,
          dump,
          print,
          diag
        } m (mode::run);

        bool print_line (false);
        optional<string> diag_name;

        for (int i (1); i != argc; ++i)
        {
          string a (argv[i]);

          if (a == "-l")
            print_line = true;
          else if (a == "-d")
            m = mode::dump;
          else if (a == "-p")
            m = mode::print;
          else if (a == "-g")
            m = mode::diag;
          else
          {
            if (m == mode::diag)
            {
              diag_name = move (a);
              break;
            }

            assert (false);
          }
        }

        assert (!print_line || m == mode::run);
        assert (!diag_name  || m == mode::diag);

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

          script s (p.pre_parse (tt,
                                 cin, nm,
                                 11 /* line */,
                                 (m != mode::diag
                                  ? optional<string> ("test")
                                  : move (diag_name)),
                                 location (nm, 10)));

          switch (m)
          {
          case mode::run:
            {
              environment e (perform_update_id, tt, false /* temp_dir */);
              print_runner r (print_line);
              p.execute (ctx.global_scope, ctx.global_scope, e, s, r);
              break;
            }
          case mode::diag:
            {
              if (s.diag_name)
              {
                cout << "name: " << *s.diag_name << endl;
              }
              else
              {
                assert (s.diag_line);

                environment e (perform_update_id, tt, false /* temp_dir */);

                cout << "diag: " << p.execute_special (ctx.global_scope,
                                                       ctx.global_scope,
                                                       e,
                                                       *s.diag_line) << endl;
              }

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
