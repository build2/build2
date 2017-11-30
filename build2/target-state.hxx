// file      : build2/target-state.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TARGET_STATE_HXX
#define BUILD2_TARGET_STATE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

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

  ostream&
  operator<< (ostream&, target_state); // target.cxx
}

#endif // BUILD2_TARGET_STATE_HXX
