// file      : libbuild2/shell/script/parser.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/context.hxx>

#include <libbuild2/shell/script/script.hxx>
#include <libbuild2/shell/script/parser.hxx>
#include <libbuild2/shell/script/runner.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;

namespace build2
{
  namespace shell
  {
    namespace script
    {
      class print_runner: public runner
      {
      public:
        print_runner (bool line, bool iterations):
            line_ (line),
            iterations_ (iterations) {}

        virtual void
        enter (environment&, const location&) override {}

        virtual void
        run (environment& env,
             const command_expr& e,
             const iteration_index* ii, size_t i,
             const function<command_function>& cf,
             const location& ll) override
        {
          // If the functions is specified, then just execute it with an empty
          // stdin so it can perform the housekeeping (stop replaying tokens,
          // increment line index, etc).
          //
          if (cf != nullptr)
          {
            assert (e.size () == 1 && !e[0].pipe.empty ());

            const command& c (e[0].pipe.back ());

            // Must be enforced by the caller.
            //
            assert (!c.out && !c.err && !c.exit);

            cf (env, c.arguments,
                fdopen_null (), nullptr /* pipe */,
                nullopt /* deadline */,
                ll);
          }

          cout << e;

          if (line_ || iterations_)
            print_line_info (ii, i);

          cout << endl;
        }

        virtual bool
        run_cond (environment&,
                  const command_expr& e,
                  const iteration_index* ii, size_t i,
                  const location&) override
        {
          cout << "? " << e;

          if (line_ || iterations_)
            print_line_info (ii, i);

          cout << endl;

          return e.back ().pipe.back ().program.recall.string () == "true";
        }

        virtual void
        leave (environment&, const location&) override {}

      private:
        void
        print_line_info (const iteration_index* ii, size_t i) const
        {
          cout << " #";

          if (line_)
            cout << ' ' << i;

          if (iterations_ && ii != nullptr)
          {
            string s;
            for (const iteration_index* i (ii); i != nullptr; i = i->prev)
              s.insert (0, " i" + to_string (i->index));

            cout << s;
          }
        }

      private:
        bool line_;
        bool iterations_;
      };

      // Usages:
      //
      // argv[0] [-l] [-r]
      // argv[0] -b
      //
      // In the first form read the script from stdin and trace the script
      // body execution to stdout using the custom print runner.
      //
      // In the second form read the script from stdin, parse it and dump the
      // script body lines to stdout.
      //
      // -l
      //    Print the script line number for each executed expression.
      //
      // -r
      //    Print the loop iteration numbers for each executed expression.
      //
      // -b
      //    Dump the parsed script body to stdout.
      //
      int
      main (int argc, char* argv[])
      {
        tracer trace ("main");

        enum class mode
        {
          run,
          body
        } m (mode::run);

        bool print_line (false);
        bool print_iterations (false);

        for (int i (1); i != argc; ++i)
        {
          string a (argv[i]);

          if (a == "-l")
            print_line = true;
          else if (a == "-r")
            print_iterations = true;
          else if (a == "-b")
            m = mode::body;
          else
            assert (false);
        }

        assert (!print_line       || m == mode::run);
        assert (!print_iterations || m == mode::run);

        // Initialize the global state.
        //
        init (nullptr,
              argv[0],
              true    /* serial_stop  */,
              false   /* mtime_check  */,
              nullopt /* config_sub   */,
              nullopt /* config_guess */);

        context ctx (true /* no_diag_buffer */);

        try
        {
          cin.exceptions (istream::failbit | istream::badbit);

          // Parse and run.
          //
          parser p (ctx);
          path_name nm ("shellscript");
          script s (p.pre_parse (ctx.global_scope, cin, nm, 11));

          switch (m)
          {
          case mode::run:
            {
              environment e (ctx.global_scope, path(), strings ());
              print_runner r (print_line, print_iterations);

              p.execute (e, s, r);
              break;
            }
          case mode::body:
            {
              dump (cout, "", s.body, s.syntax);
              break;
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
  return build2::shell::script::main (argc, argv);
}
