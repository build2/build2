// file      : build2/target.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scope>
#include <build2/diagnostics>
#include <build2/prerequisite>

namespace build2
{
  // prerequisite_members_range
  //
  template <typename T>
  void prerequisite_members_range<T>::iterator::
  switch_mode ()
  {
    // A group could be empty, so we may have to iterate.
    //
    do
    {
      g_ = resolve_group_members (r_->a_, search (*i_));
      assert (g_.members != nullptr); // Group could not be resolved.

      if (g_.count != 0) // Skip empty see through groups.
      {
        j_ = 1; // Start from the first group member.
        break;
      }
    }
    while (++i_ != r_->e_ && i_->type.see_through);
  }

  //
  //
  template <const char* ext>
  optional<string>
  target_extension_fix (const target_key&, scope&, bool)
  {
    return string (ext);
  }

  template <const char* var, const char* def>
  optional<string>
  target_extension_var (const target_key& tk, scope& s, bool)
  {
    // Include target type/pattern-specific variables.
    //
    if (auto l = s.find (var, tk))
    {
      // Help the user here and strip leading '.' from the extension.
      //
      const string& e (cast<string> (l));
      return !e.empty () && e.front () == '.' ? string (e, 1) : e;
    }

    return def != nullptr ? optional<string> (def) : nullopt;
  }
}
