// file      : libbuild2/script/timeout.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCRIPT_TIMEOUT_HXX
#define LIBBUILD2_SCRIPT_TIMEOUT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

namespace build2
{
  // Parse the specified in seconds timeout returning it if the value is not
  // zero and nullopt otherwise. Issue diagnostics with an optional prefix and
  // fail if the argument is not a valid timeout.
  //
  optional<duration>
  parse_timeout (const string&,
                 const char* what,
                 const char* prefix = "",
                 const location& = location ());

  // As above, but return the timepoint which is away from now by the
  // specified timeout.
  //
  optional<timestamp>
  parse_deadline (const string&,
                  const char* what,
                  const char* prefix = "",
                  const location& = location ());

  // Return the earlier timeout/deadline of two values, if any is present.
  //
  // Note that earlier(nullopt, v) and earlier(v, nullopt) return v.
  //
  template <typename T>
  T
  earlier (const T&, const T&);

  template <typename T>
  T
  earlier (const optional<T>&, const T&);

  template <typename T>
  T
  earlier (const T&, const optional<T>&);

  template <typename T>
  optional<T>
  earlier (const optional<T>&, const optional<T>&);
}

#include <libbuild2/script/timeout.ixx>

#endif // LIBBUILD2_SCRIPT_TIMEOUT_HXX
