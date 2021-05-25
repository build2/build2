// file      : libbuild2/name.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline int name::
  compare (const name& x) const
  {
    int r (proj < x.proj ? -1 : (proj > x.proj ? 1 : 0));

    if (r == 0)
      r = dir.compare (x.dir);

    if (r == 0)
      r = type.compare (x.type);

    if (r == 0)
      r = value.compare (x.value);

    if (r == 0)
      r = pair < x.pair ? -1 : (pair > x.pair ? 1 : 0);

    if (r == 0)
    {
      bool p (pattern);
      bool xp (x.pattern);

      r = p == xp ? 0 : (p ? 1 : -1);

      if (r == 0 && p)
      {
        auto p (static_cast<uint8_t> (*pattern));
        auto xp (static_cast<uint8_t> (*x.pattern));

        r = p < xp ? -1 : (p > xp ? 1 : 0);
      }
    }

    return r;
  }

  inline name
  to_name (string s)
  {
    if (!s.empty () && path::traits_type::is_separator (s.back ()))
    {
      dir_path d (move (s), dir_path::exact);

      if (!d.empty ())
        return name (move (d));
    }

    return name (move (s));
  }
}
