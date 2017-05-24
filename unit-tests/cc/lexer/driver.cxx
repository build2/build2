// file      : unit-tests/cc/lexer/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/cc/lexer.hxx>

using namespace std;

namespace build2
{
  namespace cc
  {
    // Usage: argv[0] [<file>]
    //
    int
    main (int argc, char* argv[])
    {
      try
      {
        istream* is;
        const char* in;

        // Reading from file is several times faster.
        //
        ifdstream ifs;
        if (argc > 1)
        {
          in = argv[1];
          ifs.open (in);
          is = &ifs;
        }
        else
        {
          in = "stdin";
          cin.exceptions (istream::failbit | istream::badbit);
          is = &cin;
        }

        lexer l (*is, path (in));

        // No use printing eos since we will either get it or loop forever.
        //
        for (token t; l.next (t) != token_type::eos; )
          cout << t << endl;
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
