// file      : build2/name.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline int name::
  compare (const name& x) const
  {
    int r;

    // Project string is pooled, so for equality can just compare pointers.
    //
    r = proj == x.proj
      ? 0
      : proj == nullptr || (x.proj != nullptr && *proj < *x.proj) ? -1 : 1;

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
}
