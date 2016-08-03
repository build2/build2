// file      : build2/config/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scope>
#include <build2/context>

namespace build2
{
  namespace config
  {
    template <typename T>
    pair<reference_wrapper<const value>, bool>
    required (scope& root,
              const variable& var,
              const T& def_val,
              bool def_ovr,
              uint64_t save_flags)
    {
      // Note: see also the other required() version if changing anything
      // here.

      if (current_mif->id == configure_id)
        save_variable (root, var, save_flags);

      pair<lookup, size_t> org (root.find_original (var));

      bool n (false); // New flag.
      lookup l (org.first);

      // The interaction with command line overrides can get tricky. For
      // example, the override to defaul value could make (non-recursive)
      // command line override in the outer scope no longer apply. So what we
      // are going to do is first ignore overrides and perform the normal
      // logic on the original. Then we apply the overrides on the result.
      //
      if (!l.defined () || (def_ovr && !l.belongs (root)))
      {
        value& v (root.assign (var) = def_val);
        v.extra = true; // Default value flag.

        n = (save_flags & save_commented) == 0; // Absence means default.
        l = lookup (v, root);
        org = make_pair (l, 1); // Lookup depth is 1 since it's in root.vars.
      }
      // Treat an inherited value that was set to default as new.
      //
      else if (l->extra)
        n = (save_flags & save_commented) == 0; // Absence means default.

      if (var.override != nullptr)
      {
        pair<lookup, size_t> ovr (root.find_override (var, move (org)));

        if (l != ovr.first) // Overriden?
        {
          // Override is always treated as new.
          //
          n = true;
          l = move (ovr.first);
        }
      }

      return pair<reference_wrapper<const value>, bool> (*l, n);
    }
  }
}
