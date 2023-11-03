// file      : libbuild2/cc/lexer.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/lexer.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    // Usage: argv[0] [-l] [-f] [<file>]
    //
    // -l
    //   Print location.
    //
    // -f
    //   Print first flag.
    //
    int
    main (int argc, char* argv[])
    {
      bool loc (false);
      bool first (false);
      path file;

      for (int i (1); i != argc; ++i)
      {
        string a (argv[i]);

        if (a == "-l")
          loc = true;
        else if (a == "-f")
          first = true;
        else
        {
          file = path (argv[i]);
          break;
        }
      }

      try
      {
        path_name in;
        ifdstream is;

        if (!file.empty ())
        {
          in = path_name (file);
          is.open (file);
        }
        else
        {
          in = path_name ("<stdin>");
          is.open (fddup (stdin_fd ()));
        }

        lexer l (is, in, true /* preprocessed */);

        // No use printing eos since we will either get it or loop forever.
        //
        for (token t; l.next (t) != token_type::eos; )
        {
          cout << t;

          if (first)
            cout << ' ' << (t.first ? 't' : 'f');

          if (loc)
            cout << ' ' << *t.file << ':' << t.line << ':' << t.column;

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

int
main (int argc, char* argv[])
{
  return build2::cc::main (argc, argv);
}
