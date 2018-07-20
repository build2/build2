// file      : build2/functions-name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function.hxx>
#include <build2/variable.hxx>

using namespace std;

namespace build2
{
  void
  name_functions ()
  {
    // function_family f ("name");

    // Name-specific overloads from builtins.
    //
    function_family b ("builtin");

    b[".concat"] = [](dir_path d, name n)
    {
      d /= n.dir;
      n.dir = move (d);
      return n;
    };
  }
}
