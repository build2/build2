// file      : build2/install/functions.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function.hxx>
#include <build2/variable.hxx>

#include <build2/install/utility.hxx>

using namespace std;

namespace build2
{
  namespace install
  {
    void
    functions ()
    {
      function_family f ("install");

      // Resolve potentially relative install.* value to an absolute directory
      // based on (other) install.* values visible from the calling scope.
      //
      f["resolve"] = &resolve_dir;
    }
  }
}
