// file      : build2/target.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scope>
#include <build2/context>     // extension_pool
#include <build2/diagnostics>
#include <build2/prerequisite>

namespace build2
{
  template <const char* ext>
  const string*
  target_extension_fix (const target_key&, scope&)
  {
    return &extension_pool.find (ext);
  }

  template <const char* var, const char* def>
  const string*
  target_extension_var (const target_key& tk, scope& s)
  {
    // Include target type/pattern-specific variables.
    //
    if (auto l = s.lookup (tk, var))
    {
      // Help the user here and strip leading '.' from the extension.
      //
      const string& e (cast<string> (*l));
      return &extension_pool.find (
        !e.empty () && e.front () == '.' ? string (e, 1) : e);
    }

    return def != nullptr ? &extension_pool.find (def) : nullptr;
  }
}
