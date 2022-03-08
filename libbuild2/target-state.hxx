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
  // Note also that value 0 is available to indicate absent/invalid state.
  //
  // NOTE: don't forget to also update operator<<(ostream,target_state) if
  //       changing anything here.
  //
  enum class target_state: uint8_t
  {
    unknown = 1,
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

  LIBBUILD2_SYMEXPORT string
  to_string (target_state); // target.cxx

  inline ostream&
  operator<< (ostream& o, target_state ts)
  {
    return o << to_string (ts);
  }
}

#endif // LIBBUILD2_TARGET_STATE_HXX
