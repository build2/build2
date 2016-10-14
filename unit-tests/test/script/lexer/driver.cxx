// file      : unit-tests/test/script/driver.cxx -*- C++ -*-
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
      int
      main (int argc, char* argv[])
      {
        // Usage: argv[0] <lexer-mode>
        //
        lexer_mode m;
        {
          assert (argc == 2);
          string s (argv[1]);

          if      (s == "script")   m = lexer_mode::script_line;
          else if (s == "variable") m = lexer_mode::variable_line;
          else if (s == "test")     m = lexer_mode::test_line;
          else if (s == "command")  m = lexer_mode::command_line;
          else if (s == "here")     m = lexer_mode::here_line;
          else                      assert (false);
        }

        try
        {
          cin.exceptions (istream::failbit | istream::badbit);
          lexer l (cin, path ("stdin"), m);

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
