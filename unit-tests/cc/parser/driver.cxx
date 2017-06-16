// file      : unit-tests/cc/parser/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/cc/parser.hxx>

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

        parser p;
        translation_unit u (p.parse (*is, path (in)));

        for (const module_import& m: u.mod.imports)
          cout << (m.exported ? "export " : "")
               << "import " << m.name << ';' << endl;

        if (!u.mod.name.empty ())
          cout << (u.mod.iface ? "export " : "")
               << "module " << u.mod.name << ';' << endl;
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
