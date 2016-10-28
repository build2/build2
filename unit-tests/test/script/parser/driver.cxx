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
        print_runner (bool scope, bool id): scope_ (scope), id_ (id) {}

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
        bool id_;
        string ind_;
      };

      // Usage: argv[0] [-s] [-i] [<testscript-name>]
      //
      int
      main (int argc, char* argv[])
      {
        tracer trace ("main");

        init ("false", 1);  // No build system driver, default verbosity.
        reset (strings ()); // No command line variables.

        bool scope (false);
        bool id (false);
        path name;

        for (int i (1); i != argc; ++i)
        {
          string a (argv[i]);

          if (a == "-s")
            scope = true;
          else if (a == "-i")
            id = true;
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
          script s (tt, st, dir_path (work) /= "test-driver");
          print_runner r (scope, id);

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
