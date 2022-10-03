// file      : libbuild2/build/script/lexer.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/build/script/token.hxx>
#include <libbuild2/build/script/lexer.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;

namespace build2
{
  namespace build
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
          else if (s == "variable")          m = lexer_mode::variable;
          else if (s == "for-loop")          m = lexer_mode::for_loop;
          else                               assert (false);
        }

        try
        {
          cin.exceptions (istream::failbit | istream::badbit);

          // Some modes auto-expire so we need something underneath.
          //
          bool u (m != lexer_mode::command_line);

          path_name in ("<stdin>");
          lexer l (cin, in, 1 /* line */, lexer_mode::command_line);
          if (u)
            l.mode (m);

          // No use printing eos since we will either get it or loop forever.
          //
          for (token t (l.next ()); t.type != token_type::eos; t = l.next ())
          {
            // Print each token on a separate line without quoting operators.
            //
            t.printer (cout, t, print_mode::normal);
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
  return build2::build::script::main (argc, argv);
}
