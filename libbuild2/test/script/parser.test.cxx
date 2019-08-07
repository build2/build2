// file      : libbuild2/test/script/parser.test.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>   // reset()
#include <libbuild2/scheduler.hxx>

#include <libbuild2/test/target.hxx>

#include <libbuild2/test/script/token.hxx>
#include <libbuild2/test/script/parser.hxx>
#include <libbuild2/test/script/runner.hxx>

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
        print_runner (bool scope, bool id, bool line)
            : scope_ (scope), id_ (id), line_ (line) {}

        virtual bool
        test (scope&) const override
        {
          return true;
        }

        virtual void
        enter (scope& s, const location&) override
        {
          if (s.desc)
          {
            const auto& d (*s.desc);

            if (!d.id.empty ())
              cout << ind_ << ": id:" << d.id << endl;

            if (!d.summary.empty ())
              cout << ind_ << ": sm:" << d.summary << endl;

            if (!d.details.empty ())
            {
              if (!d.id.empty () || !d.summary.empty ())
                cout << ind_ << ":" << endl; // Blank.

              const auto& s (d.details);
              for (size_t b (0), e (0), n; e != string::npos; b = e + 1)
              {
                e = s.find ('\n', b);
                n = ((e != string::npos ? e : s.size ()) - b);

                cout << ind_ << ':';
                if (n != 0)
                {
                  cout << ' ';
                  cout.write (s.c_str () + b, static_cast<streamsize> (n));
                }
                cout << endl;
              }
            }
          }

          if (scope_)
          {
            cout << ind_ << "{";

            if (id_ && !s.id_path.empty ()) // Skip empty root scope id.
              cout << " # " << s.id_path.string ();

            cout << endl;

            ind_ += "  ";
          }
        }

        virtual void
        run (scope&,
             const command_expr& e, command_type t,
             size_t i,
             const location&) override
        {
          const char* s (nullptr);

          switch (t)
          {
          case command_type::test:     s = "";  break;
          case command_type::setup:    s = "+"; break;
          case command_type::teardown: s = "-"; break;
          }

          cout << ind_ << s << e;

          if (line_)
            cout << " # " << i;

          cout << endl;
        }

        virtual bool
        run_if (scope&,
                const command_expr& e,
                size_t i,
                const location&) override
        {
          cout << ind_ << "? " << e;

          if (line_)
            cout << " # " << i;

          cout << endl;

          return e.back ().pipe.back ().program.string () == "true";
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
        bool id_;
        bool line_;
        string ind_;
      };

      // Usage: argv[0] [-s] [-i] [-l] [<testscript-name>]
      //
      int
      main (int argc, char* argv[])
      {
        tracer trace ("main");

        // Fake build system driver, default verbosity.
        //
        init_diag (1);
        init (nullptr, argv[0]);
        sched.startup (1);  // Serial execution.
        reset (strings ()); // No command line variables.

        bool scope (false);
        bool id (false);
        bool line (false);
        path name;

        for (int i (1); i != argc; ++i)
        {
          string a (argv[i]);

          if (a == "-s")
            scope = true;
          else if (a == "-i")
            id = true;
          else if (a == "-l")
            line = true;
          else
          {
            name = path (move (a));
            break;
          }
        }

        if (name.empty ())
          name = path ("testscript");

        assert (!id || scope); // Id can only be printed with scope.

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
                                  string (),
                                  trace));

          value& v (
            tt.assign (
              var_pool.rw ().insert<target_triplet> (
                "test.target", variable_visibility::project)));

          v = cast<target_triplet> ((*global_scope)["build.host"]);

          testscript& st (
            targets.insert<testscript> (work,
                                        dir_path (),
                                        name.leaf ().base ().string (),
                                        name.leaf ().extension (),
                                        trace));

          tt.path (path ("driver"));
          st.path (name);

          // Parse and run.
          //
          parser p;
          script s (tt, st, dir_path (work) /= "test-driver");
          p.pre_parse (cin, s);

          print_runner r (scope, id, line);
          p.execute (s, r);
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
