// file      : libbuild2/dynamic.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DYNAMIC_HXX
#define LIBBUILD2_DYNAMIC_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>
#include <libbuild2/target.hxx>

#include <libbuild2/export.hxx>

// Additional functionality that is normally only useful for implementing
// rules with dynamic dependencies.
//
namespace build2
{
  class LIBBUILD2_SYMEXPORT dyndep_rule
  {
  public:
    // Update the target during the match phase. Return true if it has changed
    // or if the passed timestamp is not timestamp_unknown and is older than
    // the target.
    //
    static bool
    update (tracer&, action, const target&, timestamp);
  };
}

#endif // LIBBUILD2_DYNAMIC_HXX
