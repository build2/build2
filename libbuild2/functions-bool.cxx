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
    // Convert a boolean value to a string literal `true` or `false`.
    //

    // Note that we don't handle NULL values for this type since it has no
    // empty representation.
    //
    f["string"] += [](bool b) {return b ? "true" : "false";};
  }
}
