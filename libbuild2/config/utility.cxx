// file      : libbuild2/config/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/utility.hxx>

using namespace std;

namespace build2
{
  void (*config_save_variable) (scope&, const variable&, optional<uint64_t>);
  void (*config_save_module) (scope&, const char*, int);
  const string& (*config_preprocess_create) (context&,
                                             values&,
                                             vector_view<opspec>&,
                                             bool,
                                             const location&);
  bool (*config_configure_post) (scope&, bool (*)(action, const scope&));
  bool (*config_disfigure_pre) (scope&, bool (*)(action, const scope&));

  namespace config
  {
    pair<lookup, bool>
    lookup_config_impl (scope& rs, const variable& var)
    {
      // This is a stripped-down version of the default value case.

      pair<lookup, size_t> org (rs.lookup_original (var));

      bool n (false); // New flag.
      lookup l (org.first);

      // Treat an inherited value that was set to default as new.
      //
      if (l.defined () && l->extra)
        n = true;

      if (var.overrides != nullptr)
      {
        // This is tricky: if we didn't find the original, pretend we have set
        // the default value for the purpose of override lookup in order to
        // have consistent semantics with the default value case (see notes in
        // that implementation for background).
        //
        // In particular, this makes sure we can first do the lookup without
        // the default value and then, if there is no value, call the version
        // with the default value and end up with the same result if we called
        // the default value version straight away.
        //
        // Note that we need to detect both when the default value is not
        // overridden as well as when the override is based on it (e.g., via
        // append; think config.cxx+=-m32).
        //
        // @@ Maybe a callback that computes the default value on demand is a
        //    better way?
        //
        variable_map::value_data v; // NULL value, but must be with version.
        if (!l.defined ())
          org = make_pair (lookup (v, var, rs), 1); // As default value case.

        scope::override_info li (rs.lookup_override_info (var, move (org)));
        pair<lookup, size_t>& ovr (li.lookup);

        if (l.defined () ? l != ovr.first : !li.original) // Overriden?
        {
          // Override is always treated as new.
          //
          n = true;
          l = move (ovr.first);
        }
      }

      if (l.defined ())
        save_variable (rs, var);

      return pair<lookup, bool> (l, n);
    }

    bool
    specified_config (scope& rs, const string& n)
    {
      // Search all outer scopes for any value in this namespace.
      //
      // What about "pure" overrides, i.e., those without any original values?
      // Well, they will also be found since their names have the original
      // variable as a prefix. But do they apply? Yes, since we haven't found
      // any original values, they will be "visible"; see find_override() for
      // details.
      //
      const variable& vns (rs.var_pool ().insert ("config." + n));
      for (scope* s (&rs); s != nullptr; s = s->parent_scope ())
      {
        for (auto p (s->vars.lookup_namespace (vns));
             p.first != p.second;
             ++p.first)
        {
          const variable& var (p.first->first);

          // Ignore config.*.configured.
          //
          if (var.name.size () < 11 ||
              var.name.compare (var.name.size () - 11, 11, ".configured") != 0)
            return true;
        }
      }

      return false;
    }

    bool
    unconfigured (scope& rs, const string& n)
    {
      // Pattern-typed as bool.
      //
      const variable& var (
        rs.var_pool ().insert ("config." + n + ".configured"));

      save_variable (rs, var);

      auto l (rs[var]); // Include inherited values.
      return l && !cast<bool> (l);
    }

    bool
    unconfigured (scope& rs, const string& n, bool v)
    {
      // Pattern-typed as bool.
      //
      const variable& var (
        rs.var_pool ().insert ("config." + n + ".configured"));

      save_variable (rs, var);

      value& x (rs.assign (var));

      if (x.null || cast<bool> (x) != !v)
      {
        x = !v;
        return true;
      }
      else
        return false;
    }
  }
}
