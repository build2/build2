// file      : libbuild2/functions-name.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_FUNCTIONS_NAME_HXX
#define LIBBUILD2_FUNCTIONS_NAME_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Helpers that may be useful to other functions that operate on target
  // name.

  // Resolve the name to target issuing diagnostics and failing if not found.
  //
  LIBBUILD2_SYMEXPORT const target&
  to_target (const scope&, name&&, name&& out);

  // As above but from the names vector which should contain a single name
  // or an out-qualified name pair (asserted).
  //
  LIBBUILD2_SYMEXPORT const target&
  to_target (const scope&, names&&);
}

#endif // LIBBUILD2_FUNCTIONS_NAME_HXX
