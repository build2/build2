// file      : libbuild2/config/utility.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace config
  {
    template <typename T>
    pair<lookup, bool>
    lookup_config_impl (scope& rs,
                        const variable& var,
                        T&& def_val,
                        uint64_t sflags,
                        bool def_ovr)
    {
      // Note: see also the other lookup_config() implementation if changing
      // anything here.

      save_variable (rs, var, sflags);

      pair<lookup, size_t> org (rs.lookup_original (var));

      bool n (false); // New flag.
      lookup l (org.first);

      // The interaction with command line overrides can get tricky. For
      // example, the override to default value could make (non-recursive)
      // command line override in the outer scope no longer apply. So what we
      // are going to do is first ignore overrides and perform the normal
      // logic on the original. Then we apply the overrides on the result.
      //
      // Note that this is not exactly the "lookup and set to default if
      // undefined" semantics in case there is no original but there is an
      // override. In this case we will set original to default and then apply
      // the override, which could be append or non-recursive (as mentioned
      // above). It does, however, feel like taking into account the default
      // in such cases is the correct semantics since append is meant as an
      // addition to something existing and non-recursive override is only
      // meant to override at the level it was specified. Though it won't be
      // surprising at all if we end up with some counter-intuitive behavior
      // here.
      //
      // Actually, the above analysis is not the full picture: if we have one
      // of those overrides (append, non-recursive) in the outer project, then
      // the lookup_config() call at that level will set the corresponding
      // variable on that scope and we will see it as "original-defined" from
      // our scope. Of course if there is no call to lookup_config() for this
      // variable in the outer scope, then we won't see anything but then our
      // behavior in this case seems correct: since that value is not part of
      // the configuration (and won't be saved), then we should stick to our
      // default. In other words, we should only inherit the value if it is
      // actually recognized as a configuration value by the outer project.
      //
      // So, to summarize the current understanding, while our semantics is
      // not exactly "lookup and set to default if undefined" in some obscure
      // corner cases, it seem to be the correct/preferred one.
      //
      if (!l.defined () || (def_ovr && !l.belongs (rs)))
      {
        value& v (rs.assign (var) = std::forward<T> (def_val)); // VC14
        v.extra = 1; // Default value flag.

        n = (sflags & save_default_commented) == 0; // Absence means default.
        l = lookup (v, var, rs);
        org = make_pair (l, 1); // Lookup depth is 1 since it's in rs.vars.
      }
      // Treat an inherited value that was set to default as new.
      //
      else if (l->extra == 1)
        n = (sflags & save_default_commented) == 0; // Absence means default.

      if (var.overrides != nullptr)
      {
        pair<lookup, size_t> ovr (rs.lookup_override (var, move (org)));

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
