// file      : build2/algorithm.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/rule>
#include <build2/prerequisite>
#include <build2/context>

namespace build2
{
  inline target&
  search (prerequisite& p)
  {
    if (p.target == nullptr)
      p.target = &search (p.key ());

    return *p.target;
  }

  inline target&
  search (const target_type& t, const prerequisite_key& k)
  {
    return search (
      prerequisite_key {
        k.proj, {&t, k.tk.dir, k.tk.out, k.tk.name, k.tk.ext}, k.scope});
  }

  inline target&
  search (const target_type& type,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext,
          scope* scope)
  {
    return search (
      prerequisite_key {nullptr, {&type, &dir, &out, &name, ext}, scope});
  }

  template <typename T>
  inline T&
  search (const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext,
          scope* scope)
  {
    return static_cast<T&> (
      search (T::static_type, dir, out, name, ext, scope));
  }

  pair<const rule*, match_result>
  match_impl (action, target&, bool apply, const rule* skip = nullptr);

  inline void
  match (action a, target& t)
  {
    if (!t.recipe (a))
      match_impl (a, t, true);

    t.dependents++;
    dependency_count++;

    // text << "M " << t << ": " << t.dependents << " " << dependency_count;
  }

  inline void
  unmatch (action, target& t)
  {
    // text << "U " << t << ": " << t.dependents << " " << dependency_count;

    assert (t.dependents != 0 && dependency_count != 0);
    t.dependents--;
    dependency_count--;
  }

  inline void
  match_only (action a, target& t)
  {
    if (!t.recipe (a))
      match_impl (a, t, false);
  }

  inline pair<recipe, action>
  match_delegate (action a, target& t, const rule& r)
  {
    auto rp (match_impl (a, t, false, &r));
    const match_result& mr (rp.second);
    return make_pair (rp.first->apply (mr.recipe_action, t), mr.recipe_action);
  }

  group_view
  resolve_group_members_impl (action, target&);

  inline group_view
  resolve_group_members (action a, target& g)
  {
    group_view r (g.group_members (a));
    return r.members != nullptr ? r : resolve_group_members_impl (a, g);
  }

  inline void
  search_and_match_prerequisites (action a, target& t)
  {
    search_and_match_prerequisites (
      a,
      t,
      (a.operation () != clean_id
       ? dir_path ()
       : t.root_scope ().out_path ()));
  }

  inline void
  search_and_match_prerequisite_members (action a, target& t)
  {
    if (a.operation () != clean_id)
      search_and_match_prerequisite_members (a, t, dir_path ());
    else
      // Note that here we don't iterate over members even for see-
      // through groups since the group target should clean eveything
      // up. A bit of an optimization.
      //
      search_and_match_prerequisites (a, t, t.root_scope ().out_path ());
  }

  target_state
  execute_impl (action, target&);

  inline target_state
  execute (action a, target& t)
  {
    // text << "E " << t << ": " << t.dependents << " " << dependency_count;

    if (dependency_count != 0) // Re-examination of a postponed target?
    {
      assert (t.dependents != 0);
      --t.dependents;
      --dependency_count;
    }

    // Don't short-circuit to the group state since we need to execute the
    // member's recipe to keep the dependency counts straight.
    //
    switch (target_state ts = t.state (false))
    {
    case target_state::unchanged:
    case target_state::changed:
      return ts;
    default:
      {
        // Handle the "last" execution mode.
        //
        // This gets interesting when we consider interaction with
        // groups. It seem to make sense to treat group members as
        // dependents of the group, so, for example, if we try to
        // clean the group via three of its members, only the last
        // attempt will actually execute the clean. This means that
        // when we match a group member, inside we should also match
        // the group in order to increment the dependents count. This
        // seems to be a natural requirement: if we are delegating to
        // the group, we need to find a recipe for it, just like we
        // would for a prerequisite.
        //
        // Note that below we are going to change the group state
        // to postponed. This is not a mistake: until we execute
        // the recipe, we want to keep returning postponed. And
        // once the recipe is executed, it will reset the state
        // to group (see group_action()). To put it another way,
        // the execution of this member is postponed, not of the
        // group.
        //
        // One important invariant to keep in mind: the return
        // value from execute() should always be the same as what
        // would get returned by a subsequent call to state().
        //
        if (current_mode == execution_mode::last && t.dependents != 0)
          return (t.raw_state = target_state::postponed);

        return execute_impl (a, t);
      }
    }
  }

  inline target_state
  execute_delegate (const recipe& r, action a, target& t)
  {
    return r (a, t);
  }

  inline target_state
  execute_direct (action a, target& t)
  {
    // Here we don't care about the counts so short-circuit state is ok.
    //
    switch (target_state ts = t.state ())
    {
    case target_state::unchanged:
    case target_state::changed: return ts;
    default: return execute_impl (a, t);
    }
  }

  template <typename T>
  inline T*
  execute_prerequisites (action a, target& t, const timestamp& mt)
  {
    return static_cast<T*> (execute_prerequisites (T::static_type, a, t, mt));
  }

  template <typename T>
  inline T*
  execute_prerequisites (const target_type& tt,
                         action a, target& t, const timestamp& mt)
  {
    return static_cast<T*> (execute_prerequisites (tt, a, t, mt));
  }
}
