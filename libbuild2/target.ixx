// file      : libbuild2/target.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cstring> // memcpy()

#include <libbuild2/export.hxx>

namespace build2
{
  LIBBUILD2_SYMEXPORT timestamp
  mtime (const char*); // filesystem.cxx

  // target_key
  //
  inline const string& target_key::
  effective_name (string& r, bool force_ext) const
  {
    const target_type& tt (*type);

    // Note that if the name is not empty, then we always use that, even
    // if the type is dir/fsdir.
    //
    if (name->empty () && (tt.is_a<build2::dir> () || tt.is_a<fsdir> ()))
    {
      r = dir->leaf ().string ();
    }
    // If we have the extension and the type expects the extension to be
    // always specified explicitly by the user, then add it to the name.
    //
    // Overall, we have the following cases:
    //
    // 1. Extension is fixed: man1{}.
    //
    // 2. Extension is always specified by the user: file{}.
    //
    // 3. Default extension that may be overridden by the user: hxx{}.
    //
    // 4. Extension assigned by the rule but may be overridden by the
    //    user: obje{}.
    //
    // By default we only include the extension for (2).
    //
    else if (ext && !ext->empty () &&
             (force_ext ||
              tt.fixed_extension == &target_extension_none ||
              tt.fixed_extension == &target_extension_must))
    {
      r = *name + '.' + *ext;
    }
    else
      return *name; // Use name as is.

    return r;
  }

  // rule_hints
  //
  inline const string& rule_hints::
  find (const target_type& tt, operation_id o, bool ut) const
  {
    // Look for fallback during the same iteration.
    //
    const value_type* f (nullptr);

    for (const value_type& v: map)
    {
      if (!(v.type == nullptr ? ut : tt.is_a (*v.type)))
        continue;

      if (v.operation == o)
        return v.hint;

      if (f == nullptr              &&
          v.operation == default_id &&
          (o == update_id || o == clean_id))
        f = &v;
    }

    return f != nullptr ? f->hint : empty_string;
  }

  inline void rule_hints::
  insert (const target_type* tt, operation_id o, string h)
  {
    auto i (find_if (map.begin (), map.end (),
                     [tt, o] (const value_type& v)
                     {
                       return v.operation == o && v.type == tt;
                     }));

    if (i == map.end ())
      map.push_back (value_type {tt, o, move (h)});
    else
      i->hint = move (h);
  }

  inline const string& target::
  find_hint (operation_id o) const
  {
    using flag = target_type::flag;

    const target_type& tt (type ());

    // First check the target itself.
    //
    if (!rule_hints.empty ())
    {
      // If this is a group that "gave" its untyped hints to the members, then
      // ignore untyped entries.
      //
      bool ut ((tt.flags & flag::member_hint) != flag::member_hint);

      const string& r (rule_hints.find (tt, o, ut));
      if (!r.empty ())
        return r;
    }

    // Then check the group.
    //
    if (const target* g = group)
    {
      if (!g->rule_hints.empty ())
      {
        // If the group "gave" its untyped hints to the members, then don't
        // ignore untyped entries.
        //
        bool ut ((g->type ().flags & flag::member_hint) == flag::member_hint);

        return g->rule_hints.find (tt, o, ut);
      }
    }

    return empty_string;
  }

  // match_extra
  //
  inline void match_extra::
  reinit (bool f)
  {
    clear_data ();
    fallback = f;
    cur_options = all_options;
    new_options = 0;
    posthoc_prerequisite_targets = nullptr;
  }

  inline void match_extra::
  free ()
  {
    clear_data ();
  }

  // target
  //
  inline const string* target::
  ext_locked () const
  {
    return *ext_ ? &**ext_ : nullptr;
  }

  inline const string* target::
  ext () const
  {
    slock l (ctx.targets.mutex_);
    return ext_locked ();
  }

  inline target_key target::
  key () const
  {
    const string* e (ext ());
    return target_key {
      &type (),
      &dir,
      &out,
      &name,
      e != nullptr ? optional<string> (*e) : nullopt};
  }

  inline target_key target::
  key_locked () const
  {
    const string* e (ext_locked ());
    return target_key {
      &type (),
      &dir,
      &out,
      &name,
      e != nullptr ? optional<string> (*e) : nullopt};
  }

  inline names target::
  as_name () const
  {
    return key ().as_name ();
  }

  inline void target::
  as_name (names& r) const
  {
    return key ().as_name (r);
  }

  inline auto target::
  prerequisites () const -> const prerequisites_type&
  {
    return prerequisites_state_.load (memory_order_acquire) == 2
      ? prerequisites_
      : empty_prerequisites_;
  }

  inline bool target::
  prerequisites (prerequisites_type&& p) const
  {
    target& x (const_cast<target&> (*this)); // MT-aware.

    uint8_t e (0);
    if (x.prerequisites_state_.compare_exchange_strong (
          e,
          1,
          memory_order_acq_rel,
          memory_order_acquire))
    {
      x.prerequisites_ = move (p);
      x.prerequisites_state_.fetch_add (1, memory_order_release);
      return true;
    }
    else
    {
      // Spin the transition out so that prerequisites() doesn't return empty.
      //
      for (; e == 1; e = prerequisites_state_.load (memory_order_acquire))
        /*this_thread::yield ()*/ ;

      return false;
    }
  }

  inline bool target::
  matched (action a, memory_order mo) const
  {
    assert (ctx.phase == run_phase::match ||
            ctx.phase == run_phase::execute);

    const opstate& s (state[a]);
    size_t c (s.task_count.load (mo));
    size_t b (ctx.count_base ()); // Note: cannot do (c - b)!

    if (ctx.phase == run_phase::match)
    {
      // While it will normally be applied, it could also be already executed
      // or being relocked to reapply match options (see lock_impl() for
      // background).
      //
      // Note that we can't just do >= offset_applied since offset_busy can
      // also mean it is being matched.
      //
      // See also matched_state_impl(), mtime() for similar logic.
      //
      return (c == (b + offset_applied)  ||
              c == (b + offset_executed) ||
              (c >= (b + offset_busy)    &&
               s.match_extra.cur_options_.load (memory_order_relaxed) != 0));
    }
    else
    {
      // Note that while the target could be being executed, we should see at
      // least offset_matched since it must have been "achieved" before the
      // phase switch.
      //
      return c >= (b + offset_matched);
    }
  }

  inline bool target::
  group_state (action a) const
  {
    // We go an extra step and short-circuit to the target state even if the
    // raw state is not group provided the recipe is group_recipe and the
    // state is unknown (see mtime() for a discussion on why we do it).
    //
    // Note that additionally s.state may not be target_state::group even
    // after execution due to deferment (see execute_impl() for details).
    //
    // @@ Hm, I wonder why not just return s.recipe_group_action now that we
    //    cache it.
    //

    // This special hack allows us to do things like query an ad hoc member's
    // state or mtime without matching/executing the member, only the group.
    // Requiring matching/executing the member would be too burdensome and
    // this feels harmless (ad hoc membership cannot be changed during the
    // execute phase).
    //
    // Note: this test must come first since the member may not be matched and
    // thus its state uninitialized.
    //
    if (ctx.phase == run_phase::execute && adhoc_group_member ())
      return true;

    const opstate& s (state[a]);

    if (s.state == target_state::group)
      return true;

    if (s.state == target_state::unknown && group != nullptr)
      return s.recipe_group_action;

    return false;
  }

  inline pair<bool, target_state> target::
  matched_state_impl (action a) const
  {
    // Note that the "tried" state is "final".
    //
    const opstate& s (state[a]);

    // Note: already synchronized.
    //
    size_t c (s.task_count.load (memory_order_relaxed));
    size_t b (ctx.count_base ()); // Note: cannot do (c - b)!

    if (c == (b + offset_tried))
      return make_pair (false, target_state::unknown);
    else
    {
      // The same semantics as in target::matched(). Note that in the executed
      // case we are guaranteed to be synchronized since we are in the match
      // phase.
      //
      assert (c == (b + offset_applied)  ||
              c == (b + offset_executed) ||
              (c >= (b + offset_busy)    &&
               s.match_extra.cur_options_.load (memory_order_relaxed) != 0));

      return make_pair (true, (group_state (a) ? group->state[a] : s).state);
    }
  }

  inline target_state target::
  executed_state_impl (action a) const
  {
    return (group_state (a) ? group->state : state)[a].state;
  }

  inline target_state target::
  matched_state (action a, bool fail) const
  {
    assert (ctx.phase == run_phase::match);

    // Note that the target could be being asynchronously re-matched.
    //
    pair<bool, target_state> r (matched_state_impl (a));

    if (fail && (!r.first || r.second == target_state::failed))
      throw failed ();

    return r.second;
  }

  inline pair<bool, target_state> target::
  try_matched_state (action a, bool fail) const
  {
    assert (ctx.phase == run_phase::match);

    pair<bool, target_state> r (matched_state_impl (a));

    if (fail && r.first && r.second == target_state::failed)
      throw failed ();

    return r;
  }

  inline target_state target::
  executed_state (action a, bool fail) const
  {
    assert (ctx.phase == run_phase::execute || ctx.phase == run_phase::load);

    target_state r (executed_state_impl (a));

    if (fail && r == target_state::failed)
      throw failed ();

    return r;
  }

  inline bool target::
  has_prerequisites () const
  {
    return !prerequisites ().empty ();
  }

  inline bool target::
  has_group_prerequisites () const
  {
    return has_prerequisites () ||
      (group != nullptr && group->has_prerequisites ());
  }

  inline bool target::
  unchanged (action a) const
  {
    assert (ctx.phase == run_phase::match);

    return matched_state_impl (a).second == target_state::unchanged;
  }

  inline ostream&
  operator<< (ostream& os, const target& t)
  {
    return os << t.key ();
  }

  // mark()/unmark()
  //

  // VC15 doesn't like if we use (abstract) target here.
  //
  static_assert (alignof (file) % 4 == 0, "unexpected target alignment");

  inline void
  mark (const target*& p, uint8_t m)
  {
    uintptr_t i (reinterpret_cast<uintptr_t> (p));
    i |= m & 0x03;
    p = reinterpret_cast<const target*> (i);
  }

  inline uint8_t
  marked (const target* p)
  {
    uintptr_t i (reinterpret_cast<uintptr_t> (p));
    return uint8_t (i & 0x03);
  }

  inline uint8_t
  unmark (const target*& p)
  {
    uintptr_t i (reinterpret_cast<uintptr_t> (p));
    uint8_t m (i & 0x03);

    if (m != 0)
    {
      i &= ~uintptr_t (0x03);
      p = reinterpret_cast<const target*> (i);
    }

    return m;
  }

  // include()
  //
  LIBBUILD2_SYMEXPORT include_type
  include_impl (action, const target&,
                const prerequisite&, const target*,
                lookup*);

  inline include_type
  include (action a, const target& t, const prerequisite& p, lookup* l)
  {
    // Most of the time no prerequisite-specific variables will be specified,
    // so let's optimize for that.
    //
    return p.vars.empty ()
      ? include_type (true)
      : include_impl (a, t, p, nullptr, l);
  }

  inline include_type
  include (action a, const target& t, const prerequisite_member& pm, lookup* l)
  {
    return pm.prerequisite.vars.empty ()
      ? include_type (true)
      : include_impl (a, t, pm.prerequisite, pm.member, l);
  }

  // group_prerequisites
  //
  inline group_prerequisites::
  group_prerequisites (const target& t)
      : t_ (t),
        g_ (t_.group == nullptr                 ||
            t_.group->adhoc_member != nullptr   || // Ad hoc group member.
            t_.group->prerequisites ().empty ()
            ? nullptr : t_.group)
  {
  }

  inline group_prerequisites::
  group_prerequisites (const target& t, const target* g)
      : t_ (t),
        g_ (g == nullptr                 ||
            g->prerequisites ().empty ()
            ? nullptr : g)
  {
  }

  inline auto group_prerequisites::
  begin () const -> iterator
  {
    auto& c ((g_ != nullptr ? *g_ : t_).prerequisites ());
    return iterator (&t_, g_, &c, c.begin ());
  }

  inline auto group_prerequisites::
  end () const -> iterator
  {
    auto& c (t_.prerequisites ());
    return iterator (&t_, g_, &c, c.end ());
  }

  inline size_t group_prerequisites::
  size () const
  {
    return t_.prerequisites ().size () +
      (g_ != nullptr ? g_->prerequisites ().size () : 0);
  }

  // group_prerequisites::iterator
  //
  inline auto group_prerequisites::iterator::
  operator++ () -> iterator&
  {
    if (++i_ == c_->end () && c_ != &t_->prerequisites ())
    {
      c_ = &t_->prerequisites ();
      i_ = c_->begin ();
    }
    return *this;
  }


  inline auto group_prerequisites::iterator::
  operator-- () -> iterator&
  {
    if (i_ == c_->begin () && c_ == &t_->prerequisites ())
    {
      c_ = &g_->prerequisites ();
      i_ = c_->end ();
    }

    --i_;
    return *this;
  }

  // prerequisite_member
  //
  inline prerequisite prerequisite_member::
  as_prerequisite () const
  {
    if (member == nullptr)
      return prerequisite;

    // An ad hoc group member cannot be used as a prerequisite (use the whole
    // group instead).
    //
    assert (!member->adhoc_group_member ());

    // Feels like copying the prerequisite's variables to member is more
    // correct than not (consider for_install, for example).
    //
    prerequisite_type p (*member);
    p.vars = prerequisite.vars;
    return p;
  }

  inline prerequisite_key prerequisite_member::
  key () const
  {
    return member != nullptr
      ? prerequisite_key {prerequisite.proj, member->key (), nullptr}
      : prerequisite.key ();
  }

  // prerequisite_members
  //
  LIBBUILD2_SYMEXPORT group_view
  resolve_members (action, const target&); // <libbuild2/algorithm.hxx>

  template <typename T>
  inline group_view prerequisite_members_range<T>::iterator::
  resolve_members (const prerequisite& p)
  {
    // We want to allow iteration over members during execute provided the
    // same iteration has been performed during match.
    //
    const target* pt (r_->t_.ctx.phase == run_phase::match
                      ? &search (r_->t_, p)
                      : search_existing (p));

    assert (pt != nullptr);

    return build2::resolve_members (r_->a_, *pt);
  }

  template <typename T>
  inline void prerequisite_members_range<T>::iterator::
  switch_mode ()
  {
    g_ = resolve_members (*i_);

    if (g_.members != nullptr)
    {
      // See empty see through groups as groups.
      //
      for (j_ = 1; j_ <= g_.count && g_.members[j_ - 1] == nullptr; ++j_) ;

      if (j_ > g_.count)
        g_.count = 0;
    }
    else
      assert (r_->mode_ != members_mode::always); // Group can't be resolved.
  }

  template <typename T>
  inline auto prerequisite_members_range<T>::iterator::
  operator++ () -> iterator&
  {
    if (k_ != nullptr) // Iterating over an ad hoc group.
      k_ = k_->adhoc_member;

    if (k_ == nullptr && g_.count != 0) // Iterating over a normal group.
    {
      if (g_.members == nullptr) // Special case, see leave_group().
        g_.count = 0;
      else
      {
        for (++j_; j_ <= g_.count && g_.members[j_ - 1] == nullptr; ++j_) ;
        if (j_ > g_.count)
          g_.count = 0;
      }
    }

    if (k_ == nullptr && g_.count == 0) // Iterating over the range.
    {
      ++i_;

      if (r_->mode_ != members_mode::never &&
          i_ != r_->e_                     &&
          i_->type.see_through ())
        switch_mode ();
    }

    return *this;
  }

  template <typename T>
  inline bool prerequisite_members_range<T>::iterator::
  enter_group ()
  {
    assert (k_ == nullptr); // No nested ad hoc group entering.

    // First see if we are about to enter an ad hoc group.
    //
    const target* t (g_.count != 0
                     ? j_ != 0 ? g_.members[j_ - 1] : nullptr
                     : i_->target.load (memory_order_consume));

    if (t != nullptr && t->adhoc_member != nullptr)
      k_ = t; // Increment that follows will make it t->member.
    else
    {
      // Otherwise assume it is a normal group.
      //
      g_ = resolve_members (*i_);

      if (g_.members == nullptr) // Members are not know.
      {
        g_.count = 0;
        return false;
      }

      // Note: 0-based to account for the increment that will follow.
      //
      for (j_ = 0; j_ != g_.count && g_.members[j_] == nullptr; ++j_) ;
      if (j_ == g_.count)
        g_.count = 0;
    }

    return true;
  }

  template <typename T>
  inline void prerequisite_members_range<T>::iterator::
  leave_group ()
  {
    if (k_ != nullptr)
    {
      // Skip until the last element (next increment will reach the end).
      //
      for (; k_->adhoc_member != nullptr; k_ = k_->adhoc_member) ;
    }
    else
    {
      // Pretend we are on the last member of a normal group.
      //
      j_ = 0;
      g_.count = 1;
      g_.members = nullptr; // Ugly "special case signal" for operator++.
    }
  }

  template <typename T>
  inline bool prerequisite_members_range<T>::iterator::
  group () const
  {
    // Ad hoc.
    //
    if (k_ != nullptr)
      return k_->adhoc_member;

    // Explicit.
    //
    if (g_.count != 0 && g_.members != nullptr)
    {
      size_t j (j_ + 1);
      for (; j <= g_.count && g_.members[j - 1] == nullptr; ++j) ;
      return j <= g_.count;
    }

    return false;
  }

  inline auto
  prerequisite_members (action a, const target& t, members_mode m)
  {
    return prerequisite_members (a, t, t.prerequisites (), m);
  }

  inline auto
  reverse_prerequisite_members (action a, const target& t, members_mode m)
  {
    return prerequisite_members (a, t, reverse_iterate (t.prerequisites ()), m);
  }

  // mtime_target
  //
  inline void mtime_target::
  mtime (timestamp mt) const
  {
    mtime_.store (mt.time_since_epoch ().count (), memory_order_release);
  }

  inline timestamp mtime_target::
  load_mtime (const path& p) const
  {
    // We can only enforce "not group state" during the execute phase. During
    // match (e.g., the target is being matched), we will just have to pay
    // attention.
    //
    assert (ctx.phase == run_phase::match ||
            (ctx.phase == run_phase::execute &&
             !group_state (action () /* inner */)));

    duration::rep r (mtime_.load (memory_order_consume));
    if (r == timestamp_unknown_rep)
    {
      assert (!p.empty ());

      r = build2::mtime (p.string ().c_str ()).time_since_epoch ().count ();
      mtime_.store (r, memory_order_release);
    }

    return timestamp (duration (r));
  }

  inline bool mtime_target::
  newer (timestamp mt, target_state s) const
  {
    assert (s != target_state::unknown); // Should be executed.

    timestamp mp (mtime ());

    // What do we do if timestamps are equal? This can happen, for example,
    // on filesystems that don't have subsecond resolution. There is not
    // much we can do here except detect the case where the target was
    // changed on this run.
    //
    return mt < mp || (mt == mp && s == target_state::changed);
  }

  inline bool mtime_target::
  newer (timestamp mt) const
  {
    assert (ctx.phase == run_phase::execute);
    return newer (mt, executed_state_impl (action () /* inner */));
  }

  // path_target
  //
  inline const path& path_target::
  path (memory_order mo) const
  {
    // You may be wondering why don't we spin the transition out? The reason
    // is it shouldn't matter since were we called just a moment earlier, we
    // wouldn't have seen it.
    //
    return path_state_.load (mo) == 2 ? path_ : empty_path;
  }

  inline const path& path_target::
  path (path_type p) const
  {
    uint8_t e (0);
    if (path_state_.compare_exchange_strong (
          e,
          1,
          memory_order_acq_rel,
          memory_order_acquire))
    {
      path_ = move (p);
      path_state_.fetch_add (1, memory_order_release);
    }
    else
    {
      // Spin the transition out.
      //
      for (; e == 1; e = path_state_.load (memory_order_acquire))
        /*this_thread::yield ()*/ ;

      assert (e == 2 && path_ == p);
    }

    return path_;
  }

  inline timestamp path_target::
  load_mtime () const
  {
    return mtime_target::load_mtime (path ());
  }

  inline const path& path_target::
  path_mtime (path_type p, timestamp mt) const
  {
    // Because we use the presence of mtime to indicate the special "trust me,
    // this file exists" situation, the order in which we do things is
    // important. In particular, the fallback file_rule::match() will skip
    // assigning the path if there is a valid timestamp. As a result, with the
    // wrong order we may end up in a situation where the rule is matched but
    // the path is not assigned.
    //
    const path_type& r (path (move (p)));
    mtime (mt);
    return r;
  }

  // exe
  //
  inline auto exe::
  process_path () const -> process_path_type
  {
    // It's unfortunate we have to return by value but hopefully the
    // compiler will see through it. Note also that returning empty
    // process path if path is empty.
    //
    return process_path_.empty ()
      ? process_path_type (path ().string ().c_str (),
                           path_type (),
                           path_type ())
      : process_path_type (process_path_, false /* init */);
  }

  inline void exe::
  process_path (process_path_type p)
  {
    process_path_ = move (p);
  }
}
