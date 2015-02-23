// file      : build/algorithm.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  void
  match_impl (target&);

  inline void
  match (target& t)
  {
    if (!t.recipe ())
      match_impl (t);
  }
}
