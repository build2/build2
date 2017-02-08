// file      : build2/target.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

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

  inline target_state target::
  atomic_state () const
  {
    switch (task_count)
    {
    case target::count_unexecuted: return target_state::unknown;
    case target::count_executed:   return synchronized_state ();
    default:                       return target_state::busy;
    }
  }

  inline target_state target::
  synchronized_state () const
  {
    // We go an extra step and short-circuit to the target state even if the
    // raw state is not group provided the recipe is group_recipe.

    if (state_ == target_state::group)
      return group->state_;

    if (group == nullptr)
      return state_;

    if (recipe_function* const* f = recipe_.target<recipe_function*> ())
    {
      if (*f == &group_action)
        return group->state_;
    }

    return state_;
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
  resolve_group_members (slock&, action, target&); // <build2/algorithm>

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
      target* t (g_.count != 0
                 ? j_ != 0 ? g_.members[j_ - 1] : nullptr // enter_group()
                 : i_->target);
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
    target* t (g_.count != 0
               ? j_ != 0 ? g_.members[j_ - 1] : nullptr
               : i_->target);

    if (t != nullptr && t->member != nullptr)
      k_ = t->member;
    else
    {
      // Otherwise assume it is a normal group.
      //
      g_ = resolve_group_members (r_->l_, r_->a_, search (*i_));

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
      target* t (g_.count != 0
                 ? j_ != 0 ? g_.members[j_ - 1] : nullptr
                 : i_->target);
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
}
