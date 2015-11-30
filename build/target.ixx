// file      : build/target.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build
{
  // prerequisite_ref
  //
  inline bool prerequisite_ref::
  belongs (const target& t) const
  {
    const auto& p (t.prerequisites);
    return !(p.empty () || this < &p.front () || this > &p.back ());
  }

  // prerequisite_member
  //
  inline prerequisite& prerequisite_member::
  as_prerequisite (tracer& trace) const
  {
    if (target == nullptr)
      return prerequisite;

    // The use of the group's prerequisite scope is debatable.
    //
    scope& s (prerequisite.get ().scope);
    return s.prerequisites.insert (nullptr, key ().tk, s, trace).first;
  }

  // prerequisite_members
  //
  group_view
  resolve_group_members (action, target&); // <build/algorithm>

  template <typename T>
  inline auto prerequisite_members_range<T>::iterator::
  operator++ () -> iterator&
  {
    if (g_.count != 0)
    {
      if (++j_ <= g_.count)
        return *this;

      // Switch back to prerequisite iteration mode.
      //
      g_.count = 0;
    }

    ++i_;

    // Switch to member iteration mode.
    //
    if (r_->members_ && i_ != r_->e_ && i_->get ().type.see_through)
    {
      bool r (switch_members ());
      assert (r); // Group could not be resolved.
    }

    return *this;
  }

  template <typename T>
  inline bool prerequisite_members_range<T>::iterator::
  switch_members ()
  {
    do
    {
      g_ = resolve_group_members (r_->a_, search (*i_));

      // If members are not know, iterate over the group as itself.
      //
      if (g_.members == nullptr)
      {
        g_.count = 0;
        return false;
      }
    }
    while (g_.count == 0  && // Skip empty groups.
           ++i_ != r_->e_ &&
           i_->get ().type.see_through);

    j_ = 1; // Start from the first group member.
    return true;
  }
}
