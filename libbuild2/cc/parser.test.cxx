// file      : libbuild2/cc/parser.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/cc/parser.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace butl;

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
        path file;

        path_name in;
        ifdstream is;

        if (argc > 1)
        {
          file = path (argv[1]);

          in = path_name (file);
          is.open (file);
        }
        else
        {
          in = path_name ("<stdin>");
          is.open (fddup (stdin_fd ()));
        }

        parser p;
        unit u (p.parse (is, in, compiler_id (compiler_type::gcc, "")));

        switch (u.type)
        {
        case unit_type::module_intf:
        case unit_type::module_intf_part:
          cout << "export ";
          // Fall through.
        case unit_type::module_impl:
        case unit_type::module_impl_part:
          cout << "module " << u.module_info.name << ';' << endl;
          break;
        default:
          break;
        }

        for (const module_import& m: u.module_info.imports)
          cout << (m.exported ? "export " : "")
               << "import " << m.name << ';' << endl;
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
