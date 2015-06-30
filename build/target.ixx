// file      : build/target.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/scope>

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
    return s.prerequisites.insert (key ().tk, s, trace).first;
  }

  // prerequisite_members
  //
  group_view
  resolve_group_members (action, target_group&); // <build/algorithm>

  template <typename T>
  inline auto prerequisite_members_range<T>::iterator::
  operator++ () -> iterator&
  {
    if (g_.count != 0)
    {
      // Member iteration.
      //
      if (++j_ == g_.count)
      {
        // Switch back to prerequisite iteration.
        //
        g_.count = 0;
        ++i_;
      }
    }
    else
    {
      // Prerequisite iteration.
      //
      if (i_->get ().template is_a<target_group> ())
      {
        // Switch to member iteration.
        //
        target_group& g (static_cast<target_group&> (search (*i_)));
        j_ = 0;
        g_ = resolve_group_members (a_, g);

        if (g_.count == 0)
          ++i_; // Empty group.
      }
      else
        ++i_;
    }

    return *this;
  }
}
