// file      : build/algorithm.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/prerequisite>
#include <build/context>

namespace build
{
  inline target&
  search (prerequisite& p)
  {
    if (p.target == nullptr)
      p.target = &search (p.key ());

    return *p.target;
  }

  inline target&
  search (const target_type& type,
          const dir_path& dir,
          const std::string& name,
          const std::string* ext,
          scope* scope)
  {
    return search (prerequisite_key {{&type, &dir, &name, &ext}, scope});
  }

  template <typename T>
  inline T&
  search (const dir_path& dir,
          const std::string& name,
          const std::string* ext,
          scope* scope)
  {
    return static_cast<T&> (search (T::static_type, dir, name, ext, scope));
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
    default:
      {
        // Handle the "last" execution mode.
        //
        if (current_mode == execution_mode::last && t.dependents != 0)
          return (t.state = target_state::postponed);

        return execute_impl (a, t);
      }
    }
  }
}
