// file      : libbuild2/lexer.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/token.hxx>
#include <libbuild2/lexer.hxx>

using namespace std;

namespace build2
{
  // Usage: argv[0] [-q] [<lexer-mode>]
  //
  int
  main (int argc, char* argv[])
  {
    bool quote (false);
    lexer_mode m (lexer_mode::normal);

    for (int i (1); i != argc; ++i)
    {
      string a (argv[i]);

      if (a == "-q")
        quote = true;
      else
      {
        if      (a == "normal")     m = lexer_mode::normal;
        else if (a == "variable")   m = lexer_mode::variable;
        else if (a == "value")      m = lexer_mode::value;
        else if (a == "attributes") m = lexer_mode::attributes;
        else if (a == "eval")       m = lexer_mode::eval;
        else if (a == "buildspec")  m = lexer_mode::buildspec;
        else                       assert (false);
        break;
      }
    }

    try
    {
      cin.exceptions (istream::failbit | istream::badbit);

      // Most alternative modes auto-expire so we need something underneath.
      //
      path_name in ("<stdin>");
      lexer l (cin, in);

      if (m != lexer_mode::normal)
        l.mode (m);

      // No use printing eos since we will either get it or loop forever.
      //
      for (token t (l.next ()); t.type != token_type::eos; t = l.next ())
      {
        if (t.separated && t.type != token_type::newline)
          cout << ' ';

        // Print each token on a separate line without quoting operators.
        //
        t.printer (cout, t, print_mode::normal);

        if (quote)
        {
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

int
main (int argc, char* argv[])
{
  return build2::main (argc, argv);
}
