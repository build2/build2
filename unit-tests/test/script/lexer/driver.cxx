// file      : unit-tests/test/script/lexer/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <build2/types>
#include <build2/utility>

#include <build2/test/script/token>
#include <build2/test/script/lexer>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      // Usage: argv[0] <lexer-mode>
      //
      int
      main (int argc, char* argv[])
      {
        lexer_mode m;
        {
          assert (argc == 2);
          string s (argv[1]);

          if      (s == "script-line")   m = lexer_mode::script_line;
          else if (s == "assign-line")   m = lexer_mode::assign_line;
          else if (s == "variable-line") m = lexer_mode::variable_line;
          else if (s == "command-line")  m = lexer_mode::command_line;
          else if (s == "here-line")     m = lexer_mode::here_line;
          else if (s == "variable")      m = lexer_mode::variable;
          else                           assert (false);
        }

        try
        {
          cin.exceptions (istream::failbit | istream::badbit);

          // The variable mode auto-expires so we need something underneath.
          //
          bool u (m == lexer_mode::variable);
          lexer l (cin, path ("stdin"), u ? lexer_mode::script_line : m);
          if (u)
            l.mode (m);

          // No use printing eos since we will either get it or loop forever.
          //
          for (token t (l.next ()); t.type != token_type::eos; t = l.next ())
          {
            // Print each token on a separate line without quoting operators.
            //
            t.printer (cout, t, false);
            cout << endl;
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
  return build2::test::script::main (argc, argv);
}
