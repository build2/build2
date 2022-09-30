// file      : libbuild2/functions-bool.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  bool_functions (function_map& m)
  {
    function_family f (m, "bool");

    // $string(<bool>)
    //
    f["string"] += [](bool b) {return b ? "true" : "false";};
  }
}
