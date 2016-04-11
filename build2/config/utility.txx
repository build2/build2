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
    required (scope& root, const variable& var, const T& def_val, bool def_ovr)
    {
      if (current_mif->id == configure_id)
        save_variable (root, var);

      pair<lookup, size_t> org (root.find_original (var));
      lookup l (org.first);
      bool n (false);

      // The interaction with command line overrides can get tricky. For
      // example, the override to defaul value could make (non-recursive)
      // command line override in the outer scope no longer apply. So what we
      // are going to do is first ignore overrides and perform the normal
      // logic on the original. Then we apply the overrides on the result.
      //
      if (!l.defined () || (def_ovr && !l.belongs (root)))
      {
        l = lookup ((root.assign (var) = def_val), root);
        org = make_pair (l, 1); // Lookup depth is 1 since in root.vars.
        n = true;
      }

      if (var.override != nullptr)
      {
        pair<lookup, size_t> ovr (root.find_override (var, move (org)));

        if (l != ovr.first) // Overriden?
        {
          l = move (ovr.first);

          // Overriden and not inherited (same logic as in save_config()).
          //
          n = l.belongs (root) || l.belongs (*global_scope);
        }
      }

      return pair<reference_wrapper<const value>, bool> (*l, n);
    }
  }
}
