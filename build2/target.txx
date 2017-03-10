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

      // Group could not be resolved.
      //
      if (g_.members == nullptr)
      {
        assert (r_->mode_ != members_mode::always);
        return;
      }

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
  target_extension_fix (const target_key&, const scope&, bool)
  {
    return string (ext);
  }

  template <const char* ext>
  bool
  target_pattern_fix (const target_type&, const scope&, string& v, bool r)
  {
    size_t p (path::traits::find_extension (v));

    if (r)
    {
      // If we get called to reverse then it means we've added the extension
      // in the first place. So simply strip it.
      //
      assert (p != string::npos);
      v.resize (p);
    }
    //
    // We only add our extension if there isn't one already.
    //
    else if (p == string::npos)
    {
      v += '.';
      v += ext;
      return true;
    }

    return false;
  }

  inline optional<string>
  target_extension_var (const target_type& tt,
                        const string& tn,
                        const scope& s,
                        const char* var,
                        const char* def)
  {
    // Include target type/pattern-specific variables.
    //
    if (auto l = s.find (var_pool[var], tt, tn))
    {
      // Help the user here and strip leading '.' from the extension.
      //
      const string& e (cast<string> (l));
      return !e.empty () && e.front () == '.' ? string (e, 1) : e;
    }

    return def != nullptr ? optional<string> (def) : nullopt;
  }

  template <const char* var, const char* def>
  optional<string>
  target_extension_var (const target_key& tk, const scope& s, bool)
  {
    return target_extension_var (*tk.type, *tk.name, s, var, def);
  }

  template <const char* var, const char* def>
  bool
  target_pattern_var (const target_type& tt, const scope& s, string& v, bool r)
  {
    size_t p (path::traits::find_extension (v));

    if (r)
    {
      // If we get called to reverse then it means we've added the extension
      // in the first place. So simply strip it.
      //
      assert (p != string::npos);
      v.resize (p);
    }
    //
    // We only add our extension if there isn't one already.
    //
    else if (p == string::npos)
    {
      // Use empty name as a target since we only want target type/pattern-
      // specific variables that match any target (e.g., '*' but not '*.txt').
      //
      if (auto e = target_extension_var (tt, string (), s, var, def))
      {
        if (!e->empty ()) // Don't add empty extension (means no extension).
        {
          v += '.';
          v += *e;
          return true;
        }
      }
    }

    return false;
  }
}
