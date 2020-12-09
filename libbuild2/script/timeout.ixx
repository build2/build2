// file      : libbuild2/script/timeout.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline optional<timestamp>
  parse_deadline (const string& s,
                  const char* what,
                  const char* prefix,
                  const location& l)
  {
    if (optional<duration> t = parse_timeout (s, what, prefix, l))
      return system_clock::now () + *t;
    else
      return nullopt;
  }

  template <typename T>
  inline T
  earlier (const T& x, const T& y)
  {
    return x < y ? x : y;
  }

  template <typename T>
  inline T
  earlier (const T& x, const optional<T>& y)
  {
    return y ? earlier (x, *y) : x;
  }

  template <typename T>
  inline T
  earlier (const optional<T>& x, const T& y)
  {
    return earlier (y, x);
  }

  template <typename T>
  inline optional<T>
  earlier (const optional<T>& x, const optional<T>& y)
  {
    return x ? earlier (*x, y) :
           y ? earlier (*y, x) :
               optional<T> ();
  }
}
