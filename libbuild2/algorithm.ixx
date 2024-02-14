// file      : libbuild2/algorithm.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/rule.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/scheduler.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  inline const target&
  search_custom (const prerequisite& p, const target& pt)
  {
    assert (pt.ctx.phase == run_phase::match ||
            pt.ctx.phase == run_phase::execute);

    const target* e (nullptr);
    if (!p.target.compare_exchange_strong (
          e, &pt,
          memory_order_release,
          memory_order_consume))
      assert (e == &pt);

    return pt;
  }

  inline const target&
  search (const target& t, const target_type& tt, const prerequisite_key& k)
  {
    return search (
      t,
      prerequisite_key {
        k.proj, {&tt, k.tk.dir, k.tk.out, k.tk.name, k.tk.ext}, k.scope});
  }

  inline pair<target&, ulock>
  search_locked (const target& t,
                 const target_type& tt,
                 const prerequisite_key& k)
  {
    return search_locked (
      t,
      prerequisite_key {
        k.proj, {&tt, k.tk.dir, k.tk.out, k.tk.name, k.tk.ext}, k.scope});
  }

  inline const target*
  search_existing (context& ctx,
                   const target_type& tt,
                   const prerequisite_key& k)
  {
    return search_existing (
      ctx,
      prerequisite_key {
        k.proj, {&tt, k.tk.dir, k.tk.out, k.tk.name, k.tk.ext}, k.scope});
  }

  inline const target&
  search_new (context& ctx,
              const target_type& tt,
              const prerequisite_key& k)
  {
    return search_new (
      ctx,
      prerequisite_key {
        k.proj, {&tt, k.tk.dir, k.tk.out, k.tk.name, k.tk.ext}, k.scope});
  }

  inline pair<target&, ulock>
  search_new_locked (context& ctx,
                     const target_type& tt,
                     const prerequisite_key& k)
  {
    return search_new_locked (
      ctx,
      prerequisite_key {
        k.proj, {&tt, k.tk.dir, k.tk.out, k.tk.name, k.tk.ext}, k.scope});
  }

  inline const target&
  search (const target& t,
          const target_type& type,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext,
          const scope* scope,
          const optional<project_name>& proj)
  {
    return search (
      t,
      prerequisite_key {
        proj,
        {
          &type,
          &dir, &out, &name,
          ext != nullptr ? optional<string> (*ext) : nullopt
        },
        scope});
  }

  inline pair<target&, ulock>
  search_locked (const target& t,
                 const target_type& type,
                 const dir_path& dir,
                 const dir_path& out,
                 const string& name,
                 const string* ext,
                 const scope* scope)
  {
    return search_locked (
      t,
      prerequisite_key {
        nullopt,
        {
          &type,
          &dir, &out, &name,
          ext != nullptr ? optional<string> (*ext) : nullopt
        },
        scope});
  }

  inline const target*
  search_existing (context& ctx,
                   const target_type& type,
                   const dir_path& dir,
                   const dir_path& out,
                   const string& name,
                   const string* ext,
                   const scope* scope,
                   const optional<project_name>& proj)
  {
    return search_existing (
      ctx,
      prerequisite_key {
        proj,
        {
          &type,
          &dir, &out, &name,
          ext != nullptr ? optional<string> (*ext) : nullopt
        },
        scope});
  }

  inline const target&
  search_new (context& ctx,
              const target_type& type,
              const dir_path& dir,
              const dir_path& out,
              const string& name,
              const string* ext,
              const scope* scope)
  {
    return search_new (
      ctx,
      prerequisite_key {
        nullopt,
        {
          &type,
          &dir, &out, &name,
          ext != nullptr ? optional<string> (*ext) : nullopt
        },
        scope});
  }

  inline pair<target&, ulock>
  search_new_locked (context& ctx,
                     const target_type& type,
                     const dir_path& dir,
                     const dir_path& out,
                     const string& name,
                     const string* ext,
                     const scope* scope)
  {
    return search_new_locked (
      ctx,
      prerequisite_key {
        nullopt,
        {
          &type,
          &dir, &out, &name,
          ext != nullptr ? optional<string> (*ext) : nullopt
        },
        scope});
  }

  template <typename T>
  inline const T&
  search (const target& t,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext,
          const scope* scope)
  {
    return search (
      t, T::static_type, dir, out, name, ext, scope).template as<T> ();
  }

  template <typename T>
  inline const T*
  search_existing (context& ctx,
                   const dir_path& dir,
                   const dir_path& out,
                   const string& name,
                   const string* ext,
                   const scope* scope)
  {
    const target* r (
      search_existing (
        ctx, T::static_type, dir, out, name, ext, scope));
    return r != nullptr ? &r->template as<T> () : nullptr;
  }

  LIBBUILD2_SYMEXPORT target_lock
  lock_impl (action, const target&,
             optional<scheduler::work_queue>,
             uint64_t = 0);

  LIBBUILD2_SYMEXPORT void
  unlock_impl (action, target&, size_t);

  inline target_lock::
  target_lock (action_type a, target_type* t, size_t o, bool f)
      : action (a), target (t), offset (o), first (f)
  {
    if (target != nullptr)
      prev = stack (this);
  }

  inline void target_lock::
  unstack ()
  {
    if (target != nullptr && prev != this)
    {
      const target_lock* cur (stack (prev));
      if (cur != this) // NDEBUG
        assert (cur == this);
      prev = this;
    }
  }

  inline void target_lock::
  unlock ()
  {
    if (target != nullptr)
    {
      unlock_impl (action, *target, offset);

      if (prev != this)
      {
        const target_lock* cur (stack (prev));
        if (cur != this) // NDEBUG
          assert (cur == this);
      }

      target = nullptr;
    }
  }

  inline auto target_lock::
  release () -> data
  {
    data r {action, target, offset, first};

    if (target != nullptr)
    {
      if (prev != this)
      {
        const target_lock* cur (stack (prev));
        if (cur != this) // NDEBUG
          assert (cur == this);
      }

      target = nullptr;
    }

    return r;
  }

  inline target_lock::
  ~target_lock ()
  {
    unlock ();
  }

  inline target_lock::
  target_lock (target_lock&& x) noexcept
      : action (x.action), target (x.target), offset (x.offset)
  {
    if (target != nullptr)
    {
      if (x.prev != &x)
      {
        const target_lock* cur (stack (this));
        if (cur != &x) // NDEBUG
          assert (cur == &x);
        prev = x.prev;
      }
      else
        prev = this;

      x.target = nullptr;
    }
  }

  inline target_lock& target_lock::
  operator= (target_lock&& x) noexcept
  {
    if (this != &x)
    {
      assert (target == nullptr);

      action = x.action;
      target = x.target;
      offset = x.offset;

      if (target != nullptr)
      {
        if (x.prev != &x)
        {
          const target_lock* cur (stack (this));
          if (cur != &x) // NDEBUG
            assert (cur == &x);
          prev = x.prev;
        }
        else
          prev = this;

        x.target = nullptr;
      }
    }

    return *this;
  }

  inline const target_lock*
  dependency_cycle (action a, const target& t)
  {
    const target_lock* l (target_lock::stack ());

    for (; l != nullptr; l = l->prev)
    {
      if (l->action == a && l->target == &t)
        break;
    }

    return l;
  }

  inline target_lock
  lock (action a, const target& t, bool m)
  {
    // We don't allow locking a target that has already been matched unless
    // explicitly requested by the caller.
    //
    target_lock r (lock_impl (a, t, scheduler::work_none));
    assert (!r                                 ||
            r.offset == target::offset_touched ||
            r.offset == target::offset_tried   ||
            (m && r.offset == target::offset_matched));
    return r;
  }

  inline target&
  add_adhoc_member (target& t, const target_type& tt, const char* e)
  {
    string n (t.name);

    if (e != nullptr)
    {
      n += '.';
      n += e;
    }

    return add_adhoc_member (t, tt, t.dir, t.out, move (n), nullopt /* ext */);
  }

  inline target*
  find_adhoc_member (target& g, const target_type& tt)
  {
    target* m (g.adhoc_member);
    for (; m != nullptr && !m->is_a (tt); m = m->adhoc_member) ;
    return m;
  }

  inline const target*
  find_adhoc_member (const target& g, const target_type& tt)
  {
    const target* m (g.adhoc_member);
    for (; m != nullptr && !m->is_a (tt); m = m->adhoc_member) ;
    return m;
  }

  LIBBUILD2_SYMEXPORT const rule_match*
  match_rule_impl (action, target&,
                   uint64_t options,
                   const rule* skip,
                   bool try_match = false,
                   match_extra* = nullptr);

  LIBBUILD2_SYMEXPORT recipe
  apply_impl (action, target&, const rule_match&);

  LIBBUILD2_SYMEXPORT pair<bool, target_state>
  match_impl (action, const target&,
              uint64_t options,
              size_t, atomic_count*,
              bool try_match = false);

  inline void
  match_inc_dependents (action a, const target& t)
  {
    t.ctx.dependency_count.fetch_add (1, memory_order_relaxed);
    t[a].dependents.fetch_add (1, memory_order_release);
  }

  inline target_state
  match_sync (action a, const target& t, uint64_t options, bool fail)
  {
    assert (t.ctx.phase == run_phase::match);

    target_state r (match_impl (a, t, options, 0, nullptr).second);

    if (r != target_state::failed)
      match_inc_dependents (a, t);
    else if (fail)
      throw failed ();

    return r;
  }

  inline pair<bool, target_state>
  try_match_sync (action a, const target& t, uint64_t options, bool fail)
  {
    assert (t.ctx.phase == run_phase::match);

    pair<bool, target_state> r (
      match_impl (a, t, options, 0, nullptr, true /* try_match */));

    if (r.first)
    {
      if (r.second != target_state::failed)
        match_inc_dependents (a, t);
      else if (fail)
        throw failed ();
    }

    return r;
  }

  inline pair<bool, target_state>
  match_sync (action a, const target& t, unmatch um, uint64_t options)
  {
    assert (t.ctx.phase == run_phase::match);

    target_state s (match_impl (a, t, options, 0, nullptr).second);

    if (s == target_state::failed)
      throw failed ();

    // If this is a member of the group then the state we've got is that of
    // the group, not the member, while the member has matched the group and
    // incremented its dependency counts. As a result, we cannot rely on the
    // unchanged state in this case.
    //
    switch (um)
    {
    case unmatch::none: break;
    case unmatch::unchanged:
      {
        if (s == target_state::unchanged && t.group == nullptr)
          return make_pair (true, s);

        break;
      }
    case unmatch::safe:
      {
        // Safe if unchanged or someone else is also a dependent (note that
        // we never decrement this count during match so that someone else
        // cannot change their mind).
        //
        if ((s == target_state::unchanged && t.group == nullptr) ||
            t[a].dependents.load (memory_order_relaxed) != 0)
          return make_pair (true, s);

        break;
      }
    }

    match_inc_dependents (a, t);
    return make_pair (false, s);;
  }

  inline target_state
  match_async (action a, const target& t,
               size_t sc, atomic_count& tc,
               uint64_t options,
               bool fail)
  {
    context& ctx (t.ctx);

    assert (ctx.phase == run_phase::match);
    target_state r (match_impl (a, t, options, sc, &tc).second);

    if (r == target_state::failed && fail && !ctx.keep_going)
      throw failed ();

    return r;
  }

  inline target_state
  match_complete (action a, const target& t, uint64_t options, bool fail)
  {
    return match_sync (a, t, options, fail);
  }

  inline pair<bool, target_state>
  match_complete (action a, const target& t, unmatch um, uint64_t options)
  {
    return match_sync (a, t, um, options);
  }

  inline target_state
  match_direct_sync (action a, const target& t, uint64_t options, bool fail)
  {
    assert (t.ctx.phase == run_phase::match);

    target_state r (match_impl (a, t, options, 0, nullptr).second);

    if (r == target_state::failed && fail)
      throw failed ();

    return r;
  }

  inline target_state
  match_direct_complete (action a, const target& t,
                         uint64_t options,
                         bool fail)
  {
    return match_direct_sync (a, t, options, fail);
  }

  // Clear rule match-specific target data (except match_extra).
  //
  inline void
  clear_target (action a, target& t)
  {
    target::opstate& s (t.state[a]);
    s.recipe = nullptr;
    s.recipe_keep = false;
    s.resolve_counted = false;
    s.vars.clear ();
    t.prerequisite_targets[a].clear ();
  }

  LIBBUILD2_SYMEXPORT void
  set_rule_trace (target_lock&, const rule_match*);

  inline void
  set_rule (target_lock& l, const rule_match* r)
  {
    if (l.target->ctx.trace_match == nullptr)
      (*l.target)[l.action].rule = r;
    else
      set_rule_trace (l, r);
  }

  inline void
  set_recipe (target_lock& l, recipe&& r)
  {
    target& t (*l.target);
    target::opstate& s (t[l.action]);

    s.recipe = move (r);
    s.recipe_group_action = false;

    // If this is a noop recipe, then mark the target unchanged to allow for
    // some optimizations.
    //
    recipe_function** f (s.recipe.target<recipe_function*> ());

    if (f != nullptr && *f == &noop_action)
      s.state = target_state::unchanged;
    else
    {
      s.state = target_state::unknown;

      // This gets tricky when we start considering direct execution, etc. So
      // here seems like the best place to do it.
      //
      // We also ignore the group recipe since group action means real recipe
      // is in the group and so this feels right conceptually.
      //
      // We also avoid incrementing this count twice for the same target if we
      // have both the inner and outer operations. In our model the outer
      // operation is either noop or it must delegate to the inner. While it's
      // possible the inner is noop while the outer is not, it is not very
      // likely. The alternative (trying to "merge" the count keeping track of
      // whether inner and/or outer is noop) gets hairy rather quickly.
      //
      if (f != nullptr && *f == &group_action)
        s.recipe_group_action = true;
      else
      {
        if (l.action.inner ())
          t.ctx.target_count.fetch_add (1, memory_order_relaxed);
      }
    }
  }

  inline void
  match_recipe (target_lock& l, recipe r, uint64_t options)
  {
    assert (options != 0                       &&
            l.target != nullptr                &&
            l.offset < target::offset_matched  &&
            l.target->ctx.phase == run_phase::match);

    match_extra& me ((*l.target)[l.action].match_extra);

    me.reinit (false /* fallback */);
    me.cur_options = options; // Already applied, so cur_, not new_options.
    me.cur_options_.store (me.cur_options, memory_order_relaxed);
    clear_target (l.action, *l.target);
    set_rule (l, nullptr); // No rule.
    set_recipe (l, move (r));
    l.offset = target::offset_applied;
  }

  inline void
  match_rule (target_lock& l, const rule_match& r, uint64_t options)
  {
    assert (l.target != nullptr                &&
            l.offset < target::offset_matched  &&
            l.target->ctx.phase == run_phase::match);

    match_extra& me ((*l.target)[l.action].match_extra);

    me.reinit (false /* fallback */);
    me.new_options = options;
    clear_target (l.action, *l.target);
    set_rule (l, &r);
    l.offset = target::offset_matched;
  }

  inline recipe
  match_delegate (action a, target& t,
                  const rule& dr,
                  uint64_t options,
                  bool try_match)
  {
    assert (t.ctx.phase == run_phase::match);

    // Note: we don't touch any of the t[a] state since that was/will be set
    // for the delegating rule.
    //
    const rule_match* r (match_rule_impl (a, t, options, &dr, try_match));
    return r != nullptr ? apply_impl (a, t, *r) : empty_recipe;
  }

  inline target_state
  match_inner (action a, const target& t, uint64_t options)
  {
    // In a sense this is like any other dependency.
    //
    assert (a.outer ());
    return match_sync (a.inner_action (), t, options);
  }

  inline pair<bool, target_state>
  match_inner (action a, const target& t, unmatch um, uint64_t options)
  {
    assert (a.outer ());
    return match_sync (a.inner_action (), t, um, options);
  }

  // Note: rematch is basically normal match but without the counts increment,
  // so we just delegate to match_direct_*().
  //
  inline target_state
  rematch_sync (action a, const target& t,
                uint64_t options,
                bool fail)
  {
    return match_direct_sync (a, t, options, fail);
  }

  inline target_state
  rematch_async (action a, const target& t,
                 size_t start_count, atomic_count& task_count,
                 uint64_t options,
                 bool fail)
  {
    return match_async (a, t, start_count, task_count, options, fail);
  }

  inline target_state
  rematch_complete (action a, const target& t,
                    uint64_t options,
                    bool fail)
  {
    return match_direct_complete (a, t, options, fail);
  }

  LIBBUILD2_SYMEXPORT void
  resolve_group_impl (target_lock&&);

  inline const target*
  resolve_group (action a, const target& t)
  {
    if (a.outer ())
      a = a.inner_action ();

    switch (t.ctx.phase)
    {
    case run_phase::match:
      {
        // Grab a target lock to make sure the group state is synchronized.
        //
        target_lock l (lock_impl (a, t, scheduler::work_none));

        // If the group is alrealy known or there is nothing else we can do,
        // then unlock and return.
        //
        if (t.group == nullptr && l.offset < target::offset_tried)
          resolve_group_impl (move (l));

        break;
      }
    case run_phase::execute: break;
    case run_phase::load:    assert (false);
    }

    return t.group;
  }

  inline void
  inject (action a, target& t, const target& p)
  {
    match_sync (a, p);
    t.prerequisite_targets[a].emplace_back (&p);
  }

  LIBBUILD2_SYMEXPORT void
  match_prerequisites (action, target&,
                       const match_search&,
                       const scope*,
                       bool search_only);

  LIBBUILD2_SYMEXPORT void
  match_prerequisite_members (action, target&,
                              const match_search_member&,
                              const scope*,
                              bool search_only);

  inline void
  match_prerequisites (action a, target& t, const match_search& ms)
  {
    match_prerequisites (
      a,
      t,
      ms,
      (a.operation () != clean_id || t.is_a<alias> ()
       ? nullptr
       : &t.root_scope ()),
      false);
  }

  inline void
  search_prerequisites (action a, target& t, const match_search& ms)
  {
    match_prerequisites (
      a,
      t,
      ms,
      (a.operation () != clean_id || t.is_a<alias> ()
       ? nullptr
       : &t.root_scope ()),
      true);
  }

  inline void
  match_prerequisite_members (action a, target& t,
                              const match_search_member& msm)
  {
    if (a.operation () != clean_id || t.is_a<alias> ())
      match_prerequisite_members (a, t, msm, nullptr, false);
    else
    {
      // Note that here we don't iterate over members even for see-through
      // groups since the group target should clean eveything up. A bit of an
      // optimization.
      //
      // @@ TMP: I wonder if this still holds for the new group semantics
      //         we have in Qt automoc? Also below.
      //
      match_search ms (
        msm
        ? [&msm] (action a,
                  const target& t,
                  const prerequisite& p,
                  include_type i)
        {
          return msm (a, t, prerequisite_member {p, nullptr}, i);
        }
        : match_search ());

      match_prerequisites (a, t, ms, &t.root_scope (), false);
    }
  }

  inline void
  search_prerequisite_members (action a, target& t,
                              const match_search_member& msm)
  {
    if (a.operation () != clean_id || t.is_a<alias> ())
      match_prerequisite_members (a, t, msm, nullptr, true);
    else
    {
      // Note that here we don't iterate over members even for see-through
      // groups since the group target should clean eveything up. A bit of an
      // optimization.
      //
      // @@ TMP: I wonder if this still holds for the new group semantics
      //         we have in Qt automoc? Also above.
      //
      match_search ms (
        msm
        ? [&msm] (action a,
                  const target& t,
                  const prerequisite& p,
                  include_type i)
        {
          return msm (a, t, prerequisite_member {p, nullptr}, i);
        }
        : match_search ());

      match_prerequisites (a, t, ms, &t.root_scope (), true);
    }
  }

  inline void
  match_prerequisites (action a, target& t, const scope& s)
  {
    match_prerequisites (a, t, nullptr, &s, false);
  }

  inline void
  search_prerequisites (action a, target& t, const scope& s)
  {
    match_prerequisites (a, t, nullptr, &s, true);
  }

  inline void
  match_prerequisite_members (action a, target& t, const scope& s)
  {
    match_prerequisite_members (a, t, nullptr, &s, false);
  }

  inline void
  search_prerequisite_members (action a, target& t, const scope& s)
  {
    match_prerequisite_members (a, t, nullptr, &s, true);
  }

  LIBBUILD2_SYMEXPORT target_state
  execute_impl (action, const target&, size_t, atomic_count*);

  inline target_state
  execute_sync (action a, const target& t, bool fail)
  {
    target_state r (execute_impl (a, t, 0, nullptr));

    if (r == target_state::busy)
    {
      t.ctx.sched->wait (t.ctx.count_executed (),
                        t[a].task_count,
                        scheduler::work_none);

      r = t.executed_state (a, false);
    }

    if (r == target_state::failed && fail)
      throw failed ();

    return r;
  }

  inline target_state
  execute_async (action a, const target& t,
                 size_t sc, atomic_count& tc,
                 bool fail)
  {
    target_state r (execute_impl (a, t, sc, &tc));

    if (r == target_state::failed && fail && !t.ctx.keep_going)
      throw failed ();

    return r;
  }

  inline target_state
  execute_complete (action a, const target& t)
  {
    // Note: standard operation execute() sidesteps this and calls
    //       executed_state() directly.

    context& ctx (t.ctx);

    // If the target is still busy, wait for its completion.
    //
    ctx.sched->wait (ctx.count_executed (),
                    t[a].task_count,
                    scheduler::work_none);

    return t.executed_state (a);
  }

  LIBBUILD2_SYMEXPORT target_state
  execute_direct_impl (action, const target&, size_t, atomic_count*);

  inline target_state
  execute_direct_sync (action a, const target& t, bool fail)
  {
    target_state r (execute_direct_impl (a, t, 0, nullptr));

    if (r == target_state::busy)
    {
      t.ctx.sched->wait (t.ctx.count_executed (),
                        t[a].task_count,
                        scheduler::work_none);

      r = t.executed_state (a, false);
    }

    if (r == target_state::failed && fail)
      throw failed ();

    return r;
  }

  inline target_state
  execute_direct_async (action a, const target& t,
                        size_t sc, atomic_count& tc,
                        bool fail)
  {
    target_state r (execute_direct_impl (a, t, sc, &tc));

    if (r == target_state::failed && fail && !t.ctx.keep_going)
      throw failed ();

    return r;
  }

  inline target_state
  execute_delegate (const recipe& r, action a, const target& t)
  {
    return r (a, t);
  }

  inline target_state
  execute_inner (action a, const target& t)
  {
    assert (a.outer ());
    return execute_sync (a.inner_action (), t);
  }

  inline target_state
  straight_execute_prerequisites (action a, const target& t,
                                  size_t c, size_t s)
  {
    auto& p (t.prerequisite_targets[a]);
    return straight_execute_members (a, t,
                                     p.data (),
                                     c == 0 ? p.size () - s: c,
                                     s);
  }

  inline target_state
  reverse_execute_prerequisites (action a, const target& t, size_t c)
  {
    auto& p (t.prerequisite_targets[a]);
    return reverse_execute_members (a, t,
                                    p.data (),
                                    c == 0 ? p.size () : c,
                                    p.size ());
  }

  inline target_state
  execute_prerequisites (action a, const target& t, size_t c)
  {
    return t.ctx.current_mode == execution_mode::first
      ? straight_execute_prerequisites (a, t, c)
      : reverse_execute_prerequisites (a, t, c);
  }

  inline target_state
  straight_execute_prerequisites_inner (action a, const target& t,
                                        size_t c, size_t s)
  {
    assert (a.outer ());
    auto& p (t.prerequisite_targets[a]);
    return straight_execute_members (t.ctx,
                                     a.inner_action (),
                                     t[a].task_count,
                                     p.data (),
                                     c == 0 ? p.size () - s : c,
                                     s);
  }

  inline target_state
  reverse_execute_prerequisites_inner (action a, const target& t, size_t c)
  {
    assert (a.outer ());
    auto& p (t.prerequisite_targets[a]);
    return reverse_execute_members (t.ctx,
                                    a.inner_action (),
                                    t[a].task_count,
                                    p.data (),
                                    c == 0 ? p.size () : c,
                                    p.size ());
  }

  inline target_state
  execute_prerequisites_inner (action a, const target& t, size_t c)
  {
    return t.ctx.current_mode == execution_mode::first
      ? straight_execute_prerequisites_inner (a, t, c)
      : reverse_execute_prerequisites_inner (a, t, c);
  }

  // If the first argument is NULL, then the result is treated as a boolean
  // value.
  //
  LIBBUILD2_SYMEXPORT pair<optional<target_state>, const target*>
  execute_prerequisites (const target_type*,
                         action, const target&,
                         const timestamp&, const execute_filter&,
                         size_t);

  LIBBUILD2_SYMEXPORT pair<optional<target_state>, const target*>
  reverse_execute_prerequisites (const target_type*,
                                 action, const target&,
                                 const timestamp&, const execute_filter&,
                                 size_t);

  inline optional<target_state>
  execute_prerequisites (action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    return execute_prerequisites (nullptr, a, t, mt, ef, n).first;
  }

  inline optional<target_state>
  reverse_execute_prerequisites (action a, const target& t,
                                 const timestamp& mt, const execute_filter& ef,
                                 size_t n)
  {
    return reverse_execute_prerequisites (nullptr, a, t, mt, ef, n).first;
  }

  template <typename T>
  inline pair<optional<target_state>, const T&>
  execute_prerequisites (action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    auto p (execute_prerequisites (T::static_type, a, t, mt, ef, n));
    return pair<optional<target_state>, const T&> (
      p.first, static_cast<const T&> (p.second));
  }

  inline pair<optional<target_state>, const target&>
  execute_prerequisites (const target_type& tt,
                         action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    auto p (execute_prerequisites (&tt, a, t, mt, ef, n));
    return pair<optional<target_state>, const target&> (p.first, *p.second);
  }

  template <typename T>
  inline pair<optional<target_state>, const T&>
  execute_prerequisites (const target_type& tt,
                         action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    auto p (execute_prerequisites (tt, a, t, mt, ef, n));
    return pair<optional<target_state>, const T&> (
      p.first, static_cast<const T&> (p.second));
  }

  template <typename T>
  inline target_state
  execute_members (action a, const target& t, T ts[], size_t n)
  {
    return t.ctx.current_mode == execution_mode::first
      ? straight_execute_members (a, t, ts, n, 0)
      : reverse_execute_members (a, t, ts, n, n);
  }
}
