// file      : unit-tests/test/script/lexer/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/test/script/token.hxx>
#include <build2/test/script/lexer.hxx>

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

          if      (s == "command-line")      m = lexer_mode::command_line;
          else if (s == "first-token")       m = lexer_mode::first_token;
          else if (s == "second-token")      m = lexer_mode::second_token;
          else if (s == "variable-line")     m = lexer_mode::variable_line;
          else if (s == "command-expansion") m = lexer_mode::command_expansion;
          else if (s == "here-line-single")  m = lexer_mode::here_line_single;
          else if (s == "here-line-double")  m = lexer_mode::here_line_double;
          else if (s == "description-line")  m = lexer_mode::description_line;
          else if (s == "variable")          m = lexer_mode::variable;
          else                               assert (false);
        }

        try
        {
          cin.exceptions (istream::failbit | istream::badbit);

          // Some modes auto-expire so we need something underneath.
          //
          bool u (m == lexer_mode::first_token      ||
                  m == lexer_mode::second_token     ||
                  m == lexer_mode::variable_line    ||
                  m == lexer_mode::description_line ||
                  m == lexer_mode::variable);

          lexer l (cin, path ("stdin"), u ? lexer_mode::command_line : m);
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
