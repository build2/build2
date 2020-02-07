// file      : libbuild2/target-state.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TARGET_STATE_HXX
#define LIBBUILD2_TARGET_STATE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // The order of the enumerators is arranged so that their integral values
  // indicate whether one "overrides" the other in the "merge" operator|
  // (see below).
  //
  // Note that postponed is "greater" than unchanged since it may result in
  // the changed state.
  //
  enum class target_state: uint8_t
  {
    unknown,
    unchanged,
    postponed,
    busy,
    changed,
    failed,
    group       // Target's state is the group's state.
  };

  inline target_state&
  operator |= (target_state& l, target_state r)
  {
    if (static_cast<uint8_t> (r) > static_cast<uint8_t> (l))
      l = r;

    return l;
  }

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, target_state); // target.cxx
}

#endif // LIBBUILD2_TARGET_STATE_HXX
