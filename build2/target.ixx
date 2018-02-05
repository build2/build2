// file      : build2/target.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cstring> // memcpy()

namespace build2
{
  // target
  //
  inline const string* target::
  ext () const
  {
    slock l (targets.mutex_);
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

  inline pair<bool, target_state> target::
  matched_state_impl (action a) const
  {
    assert (phase == run_phase::match);

    const opstate& s (state[a]);
    size_t o (s.task_count.load (memory_order_relaxed) - // Synchronized.
              target::count_base ());

    if (o == target::offset_tried)
      return make_pair (false, target_state::unknown);
    else
    {
      // Normally applied but can also be already executed.
      //
      assert (o == target::offset_applied || o == target::offset_executed);
      return make_pair (true, (group_state (a) ? group->state[a] : s).state);
    }
  }

  inline target_state target::
  executed_state_impl (action a) const
  {
    assert (phase == run_phase::execute);
    return (group_state (a) ? group->state : state)[a].state;
  }

  inline bool target::
  group_state (action a) const
  {
    // We go an extra step and short-circuit to the target state even if the
    // raw state is not group provided the recipe is group_recipe and the
    // state is not failed.
    //
    const opstate& s (state[a]);

    if (s.state == target_state::group)
      return true;

    if (s.state != target_state::failed && group != nullptr)
    {
      if (recipe_function* const* f = s.recipe.target<recipe_function*> ())
        return *f == &group_action;
    }

    return false;
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

  // prerequisite_members
  //
  group_view
  resolve_group_members (action, const target&); // <build2/algorithm.hxx>

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
      g_ = resolve_group_members (r_->a_, search (r_->t_, *i_));

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
      k_ != nullptr ? k_->member != nullptr                  : /* ad hoc */
      g_.count != 0 ? g_.members != nullptr && j_ < g_.count : /* normal */
      false;
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
    assert (phase == run_phase::execute &&
            !group_state (action () /* inner */));

    duration::rep r (mtime_.load (memory_order_consume));
    if (r == timestamp_unknown_rep)
    {
      assert (!p.empty ());

      r = file_mtime (p).time_since_epoch ().count ();
      mtime_.store (r, memory_order_release);
    }

    return timestamp (duration (r));
  }

  inline bool mtime_target::
  newer (timestamp mt) const
  {
    assert (phase == run_phase::execute);

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

      assert (path_ == p);
    }

    return path_;
  }
}
