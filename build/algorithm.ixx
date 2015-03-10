// file      : build/algorithm.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  void
  match_impl (action, target&);

  inline void
  match (action a, target& t)
  {
    if (!t.recipe (a))
      match_impl (a, t);
  }
}
