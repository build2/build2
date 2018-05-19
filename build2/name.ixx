// file      : build2/name.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
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

    return r;
  }

  inline name
  to_name (string s)
  {
    if (!s.empty () && path::traits::is_separator (s.back ()))
    {
      dir_path d (move (s), dir_path::exact);

      if (!d.empty ())
        return name (move (d));
    }

    return name (move (s));
  }
}
