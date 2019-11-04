// file      : libbuild2/config/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx> // current_mif

namespace build2
{
  namespace config
  {
    template <typename T>
    pair<lookup, bool>
    required (scope& root,
              const variable& var,
              T&& def_val,
              bool def_ovr,
              uint64_t save_flags)
    {
      // Note: see also omitted() if changing anything here.

      save_variable (root, var, save_flags);

      pair<lookup, size_t> org (root.find_original (var));

      bool n (false); // New flag.
      lookup l (org.first);

      // The interaction with command line overrides can get tricky. For
      // example, the override to default value could make (non-recursive)
      // command line override in the outer scope no longer apply. So what we
      // are going to do is first ignore overrides and perform the normal
      // logic on the original. Then we apply the overrides on the result.
      //
      if (!l.defined () || (def_ovr && !l.belongs (root)))
      {
        value& v (root.assign (var) = std::forward<T> (def_val)); // VC14
        v.extra = true; // Default value flag.

        n = (save_flags & save_commented) == 0; // Absence means default.
        l = lookup (v, var, root);
        org = make_pair (l, 1); // Lookup depth is 1 since it's in root.vars.
      }
      // Treat an inherited value that was set to default as new.
      //
      else if (l->extra)
        n = (save_flags & save_commented) == 0; // Absence means default.

      if (var.overrides != nullptr)
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

      return pair<lookup, bool> (l, n);
    }
  }
}
