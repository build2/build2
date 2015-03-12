// file      : build/algorithm.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  target&
  search_impl (prerequisite&);

  inline target&
  search (prerequisite& p)
  {
    return p.target != nullptr ? *p.target : search_impl (p);
  }

  void
  match_impl (action, target&);

  inline void
  match (action a, target& t)
  {
    if (!t.recipe (a))
      match_impl (a, t);

    t.dependents++;
  }

  target_state
  execute_impl (action, target&);

  inline target_state
  execute (action a, target& t)
  {
    t.dependents--;

    switch (t.state)
    {
    case target_state::unchanged:
    case target_state::changed: return t.state;
    default: return execute_impl (a, t);
    }
  }
}
