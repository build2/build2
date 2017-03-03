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

  inline target_state target::
  state () const
  {
    assert (phase == run_phase::execute);
    return group_state () ? group->state_ : state_;
  }

  inline bool target::
  group_state () const
  {
    // We go an extra step and short-circuit to the target state even if the
    // raw state is not group provided the recipe is group_recipe.

    if (state_ == target_state::group)
      return true;

    if (group != nullptr)
    {
      if (recipe_function* const* f = recipe_.target<recipe_function*> ())
        return *f == &group_action;
    }

    return false;
  }

  inline target_state target::
  matched_state (action_type a, bool fail) const
  {
    // Note that the target could be being asynchronously re-matched.
    //
    target_state r (state (a));

    if (fail && r == target_state::failed)
      throw failed ();

    return r;
  }

  inline target_state target::
  executed_state (bool fail) const
  {
    target_state r (state ());

    if (fail && r == target_state::failed)
      throw failed ();

    return r;
  }

  inline target_state target::
  serial_state (bool fail) const
  {
    //assert (sched.serial ());

    target_state r (group_state () ? group->state_ : state_);

    if (fail && r == target_state::failed)
      throw failed ();

    return r;
  }

  inline void target::
  recipe (recipe_type r)
  {
    recipe_ = move (r);

    // If this is a noop recipe, then mark the target unchanged to allow for
    // some optimizations.
    //
    state_ = target_state::unknown;

    if (recipe_function** f = recipe_.target<recipe_function*> ())
    {
      if (*f == &noop_action)
        state_ = target_state::unchanged;
    }
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
    if (target == nullptr)
      return prerequisite;

    // An ad hoc group member cannot be used as a prerequisite (use the whole
    // group instead).
    //
    assert (!target->adhoc_member ());

    return prerequisite_type (*target);
  }

  // prerequisite_members
  //
  group_view
  resolve_group_members (action, const target&); // <build2/algorithm>

  template <typename T>
  inline auto prerequisite_members_range<T>::iterator::
  operator++ () -> iterator&
  {
    if (k_ != nullptr) // Iterating over an ad hoc group.
      k_ = k_->member;
    else if (r_->members_)
    {
      // Get the target if one has been resolved and see if it's an ad hoc
      // group. If so, switch to the ad hoc mode.
      //
      const target* t (
        g_.count != 0
        ? j_ != 0 ? g_.members[j_ - 1] : nullptr // enter_group()
        : i_->target.load (memory_order_consume));

      if (t != nullptr && t->member != nullptr)
        k_ = t->member;
    }

    if (k_ == nullptr && g_.count != 0) // Iterating over a normal group.
    {
      if (g_.members == nullptr || // leave_group()
          ++j_ > g_.count)
        g_.count = 0;
    }

    if (k_ == nullptr && g_.count == 0) // Iterating over the range.
    {
      ++i_;

      if (r_->members_ && i_ != r_->e_ && i_->type.see_through)
        switch_mode ();
    }

    return *this;
  }

  template <typename T>
  inline bool prerequisite_members_range<T>::iterator::
  enter_group ()
  {
    // First see if we are about to enter an ad hoc group (the same code as in
    // operator++() above).
    //
    const target* t (g_.count != 0
                     ? j_ != 0 ? g_.members[j_ - 1] : nullptr
                     : i_->target.load (memory_order_consume));

    if (t != nullptr && t->member != nullptr)
      k_ = t->member;
    else
    {
      // Otherwise assume it is a normal group.
      //
      g_ = resolve_group_members (r_->a_, search (*i_));

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
    // First see if we are about to enter an ad hoc group (the same code as in
    // operator++() above).
    //
    if (k_ == nullptr)
    {
      const target* t (g_.count != 0
                       ? j_ != 0 ? g_.members[j_ - 1] : nullptr
                       : i_->target.load (memory_order_consume));
      if (t != nullptr && t->member != nullptr)
        k_ = t->member;
    }

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
    assert (phase == run_phase::execute && !group_state ());

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
    return mt < mp || (mt == mp && state () == target_state::changed);
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
