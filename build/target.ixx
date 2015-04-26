// file      : build/target.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  inline bool prerequisite_ref::
  belongs (const target& t) const
  {
    const auto& p (t.prerequisites);
    return !(p.empty () || this < &p.front () || this > &p.back ());
  }
}
