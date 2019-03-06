// file      : build2/cc/lexer.test.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/cc/lexer.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    // Usage: argv[0] [-l] [<file>]
    //
    int
    main (int argc, char* argv[])
    {
      bool loc (false);
      const char* file (nullptr);

      for (int i (1); i != argc; ++i)
      {
        string a (argv[i]);

        if (a == "-l")
          loc = true;
        else
        {
          file = argv[i];
          break;
        }
      }

      try
      {
        ifdstream is;
        if (file != nullptr)
          is.open (file);
        else
        {
          file = "stdin";
          is.open (fddup (stdin_fd ()));
        }

        lexer l (is, path (file));

        // No use printing eos since we will either get it or loop forever.
        //
        for (token t; l.next (t) != token_type::eos; )
        {
          cout << t;

          if (loc)
            cout << ' ' << t.file << ':' << t.line << ':' << t.column;

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
