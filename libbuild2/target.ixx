// file      : libbuild2/target.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cstring> // memcpy()

#include <libbuild2/filesystem.hxx> // mtime()

#include <libbuild2/export.hxx>

namespace build2
{
  // target
  //
  inline const string* target::
  ext () const
  {
    slock l (ctx.targets.mutex_);
    return *ext_ ? &**ext_ : nullptr;
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

  inline names target::
  as_name () const
  {
    return key ().as_name ();
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
  matched (action a) const
  {
    assert (ctx.phase == run_phase::execute);

    const opstate& s (state[a]);

    // Note that while the target could be being executed, we should see at
    // least offset_matched since it must have been "achieved" before the
    // phase switch.
    //
    size_t c (s.task_count.load (memory_order_relaxed) - ctx.count_base ());

    return c >= offset_matched;
  }

  LIBBUILD2_SYMEXPORT target_state
  group_action (action, const target&); // <libbuild2/algorithm.hxx>

  inline bool target::
  group_state (action a) const
  {
    // We go an extra step and short-circuit to the target state even if the
    // raw state is not group provided the recipe is group_recipe and the
    // state is unknown (see mtime() for a discussion on why we do it).
    //
    const opstate& s (state[a]);

    if (s.state == target_state::group)
      return true;

    if (s.state == target_state::unknown && group != nullptr)
    {
      if (recipe_function* const* f = s.recipe.target<recipe_function*> ())
        return *f == &group_action;
    }

    return false;
  }

  inline pair<bool, target_state> target::
  matched_state_impl (action a) const
  {
    assert (ctx.phase == run_phase::match);

    // Note that the "tried" state is "final".
    //
    const opstate& s (state[a]);

    // Note: already synchronized.
    //
    size_t o (s.task_count.load (memory_order_relaxed) - ctx.count_base ());

    if (o == offset_tried)
      return make_pair (false, target_state::unknown);
    else
    {
      // Normally applied but can also be already executed.
      //
      assert (o == offset_applied || o == offset_executed);
      return make_pair (true, (group_state (a) ? group->state[a] : s).state);
    }
  }

  inline target_state target::
  executed_state_impl (action a) const
  {
    assert (ctx.phase == run_phase::execute);
    return (group_state (a) ? group->state : state)[a].state;
  }

  inline target_state target::
  matched_state (action a, bool fail) const
  {
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
    pair<bool, target_state> r (matched_state_impl (a));

    if (fail && r.first && r.second == target_state::failed)
      throw failed ();

    return r;
  }

  inline target_state target::
  executed_state (action a, bool fail) const
  {
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
      (group != nullptr && !group->has_prerequisites ());
  }

  inline bool target::
  unchanged (action a) const
  {
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
  include_impl (action,
                const target&,
                const string&,
                const prerequisite&,
                const target*);

  inline include_type
  include (action a, const target& t, const prerequisite& p, const target* m)
  {
    // Most of the time this variable will not be specified, so let's optimize
    // for that.
    //
    if (p.vars.empty ())
      return true;

    const string* v (cast_null<string> (p.vars[t.ctx.var_include]));

    if (v == nullptr)
      return true;

    return include_impl (a, t, *v, p, m);
  }

  // group_prerequisites
  //
  inline group_prerequisites::
  group_prerequisites (const target& t)
      : t_ (t),
        g_ (t_.group == nullptr                 ||
            t_.group->member != nullptr         || // Ad hoc group member.
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
    assert (!member->adhoc_member ());

    return prerequisite_type (*member);
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
  inline auto prerequisite_members_range<T>::iterator::
  operator++ () -> iterator&
  {
    if (k_ != nullptr) // Iterating over an ad hoc group.
      k_ = k_->member;

    if (k_ == nullptr && g_.count != 0) // Iterating over a normal group.
    {
      if (g_.members == nullptr || // Special case, see leave_group().
          ++j_ > g_.count)
        g_.count = 0;
    }

    if (k_ == nullptr && g_.count == 0) // Iterating over the range.
    {
      ++i_;

      if (r_->mode_ != members_mode::never &&
          i_ != r_->e_                     &&
          i_->type.see_through)
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

    if (t != nullptr && t->member != nullptr)
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

      if (g_.count != 0) // Group is not empty.
        j_ = 0; // Account for the increment that will follow.
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
      for (; k_->member != nullptr; k_ = k_->member) ;
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
    return
      k_ != nullptr ? k_->member != nullptr                  : /* ad hoc   */
      g_.count != 0 ? g_.members != nullptr && j_ < g_.count : /* explicit */
      false;
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
    assert (ctx.phase == run_phase::execute &&
            !group_state (action () /* inner */));

    duration::rep r (mtime_.load (memory_order_consume));
    if (r == timestamp_unknown_rep)
    {
      assert (!p.empty ());

      r = build2::mtime (p).time_since_epoch ().count ();
      mtime_.store (r, memory_order_release);
    }

    return timestamp (duration (r));
  }

  inline bool mtime_target::
  newer (timestamp mt) const
  {
    assert (ctx.phase == run_phase::execute);

    timestamp mp (mtime ());

    // What do we do if timestamps are equal? This can happen, for example,
    // on filesystems that don't have subsecond resolution. There is not
    // much we can do here except detect the case where the target was
    // changed on this run.
    //
    return mt < mp || (mt == mp &&
                       executed_state_impl (action () /* inner */) ==
                       target_state::changed);
  }

  // path_target
  //
  inline const path& path_target::
  path () const
  {
    // You may be wondering why don't we spin the transition out? The reason
    // is it shouldn't matter since were we called just a moment earlier, we
    // would have seen it.
    //
    return path_state_.load (memory_order_acquire) == 2 ? path_ : empty_path;
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
