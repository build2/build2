// file      : libbuild2/install/functions.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/install/utility.hxx>

namespace build2
{
  namespace install
  {
    void
    functions (function_map& m)
    {
      function_family f (m, "install");

      // Resolve potentially relative install.* value to an absolute directory
      // based on (other) install.* values visible from the calling scope.
      //
      f[".resolve"] = [] (const scope* s, dir_path d)
      {
        if (s == nullptr)
          fail << "install.resolve() called out of scope" << endf;

        return resolve_dir (*s, move (d));
      };
    }
  }
}
