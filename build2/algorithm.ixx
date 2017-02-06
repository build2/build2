// file      : build2/algorithm.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/rule>
#include <build2/context>

namespace build2
{
  inline target&
  search (prerequisite& p)
  {
    assert (phase == run_phase::search_match);

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
          const scope* scope,
          const optional<string>& proj)
  {
    return search (
      prerequisite_key {
        proj,
        {
          &type,
          &dir,
          &out,
          &name,
          ext != nullptr ? optional<string> (*ext) : nullopt
        },
        scope});
  }

  template <typename T>
  inline T&
  search (const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext,
          const scope* scope)
  {
    return static_cast<T&> (
      search (T::static_type, dir, out, name, ext, scope));
  }

  pair<const rule*, match_result>
  match_impl (slock&, action, target&, bool apply, const rule* skip = nullptr);

  inline void
  match (slock& ml, action a, target& t)
  {
    assert (phase == run_phase::search_match);

    if (!t.recipe (a))
      match_impl (ml, a, t, true);

    //@@ MT
    //
    t.dependents++;
    dependency_count++;

    // text << "M " << t << ": " << t.dependents << " " << dependency_count;
  }

  inline void
  unmatch (action, target& t)
  {
    // text << "U " << t << ": " << t.dependents << " " << dependency_count;

    assert (phase == run_phase::search_match);

    //@@ MT
    //
    assert (t.dependents != 0 && dependency_count != 0);
    t.dependents--;
    dependency_count--;
  }

  inline void
  match_only (slock& ml, action a, target& t)
  {
    assert (phase == run_phase::search_match);

    if (!t.recipe (a))
      match_impl (ml, a, t, false);
  }

  inline pair<recipe, action>
  match_delegate (slock& ml, action a, target& t, const rule& r)
  {
    assert (phase == run_phase::search_match);

    auto rp (match_impl (ml, a, t, false, &r));
    const match_result& mr (rp.second);
    return make_pair (rp.first->apply (ml, mr.recipe_action, t),
                      mr.recipe_action);
  }

  group_view
  resolve_group_members_impl (slock& ml, action, target&);

  inline group_view
  resolve_group_members (slock& ml, action a, target& g)
  {
    assert (phase == run_phase::search_match);

    group_view r (g.group_members (a));
    return r.members != nullptr ? r : resolve_group_members_impl (ml, a, g);
  }

  void
  search_and_match_prerequisites (slock&, action, target&, const scope*);

  void
  search_and_match_prerequisite_members (slock&, action, target&, const scope*);

  inline void
  search_and_match_prerequisites (slock& ml, action a, target& t)
  {
    search_and_match_prerequisites (
      ml,
      a,
      t,
      (a.operation () != clean_id ? nullptr : &t.root_scope ()));
  }

  inline void
  search_and_match_prerequisite_members (slock& ml, action a, target& t)
  {
    if (a.operation () != clean_id)
      search_and_match_prerequisite_members (ml, a, t, nullptr);
    else
      // Note that here we don't iterate over members even for see-
      // through groups since the group target should clean eveything
      // up. A bit of an optimization.
      //
      search_and_match_prerequisites (ml, a, t, &t.root_scope ());
  }

  inline void
  search_and_match_prerequisites (slock& ml,
                                  action a,
                                  target& t,
                                  const scope& s)
  {
    search_and_match_prerequisites (ml, a, t, &s);
  }

  inline void
  search_and_match_prerequisite_members (slock& ml,
                                         action a,
                                         target& t,
                                         const scope& s)
  {
    search_and_match_prerequisite_members (ml, a, t, &s);
  }

  inline target_state
  execute_delegate (const recipe& r, action a, target& t)
  {
    return r (a, t);
  }

  // If the first argument is NULL, then the result is treated as a boolean
  // value.
  //
  pair<target*, target_state>
  execute_prerequisites (const target_type*,
                         action, target&,
                         const timestamp&, const prerequisite_filter&);

  inline pair<bool, target_state>
  execute_prerequisites (action a, target& t,
                         const timestamp& mt, const prerequisite_filter& pf)
  {
    auto p (execute_prerequisites (nullptr, a, t, mt, pf));
    return make_pair (p.first != nullptr, p.second);
  }

  template <typename T>
  inline pair<T*, target_state>
  execute_prerequisites (action a, target& t,
                         const timestamp& mt, const prerequisite_filter& pf)
  {
    auto p (execute_prerequisites (T::static_type, a, t, mt, pf));
    return make_pair (static_cast<T*> (p.first), p.second);
  }

  inline pair<target*, target_state>
  execute_prerequisites (const target_type& tt,
                         action a, target& t,
                         const timestamp& mt, const prerequisite_filter& pf)
  {
    return execute_prerequisites (&tt, a, t, mt, pf);
  }

  template <typename T>
  inline pair<T*, target_state>
  execute_prerequisites (const target_type& tt,
                         action a, target& t,
                         const timestamp& mt, const prerequisite_filter& pf)
  {
    auto p (execute_prerequisites (tt, a, t, mt, pf));
    return make_pair (static_cast<T*> (p.first), p.second);
  }
}
