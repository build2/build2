// file      : libbuild2/config/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/utility.hxx>

using namespace std;

namespace build2
{
  void (*config_save_variable) (scope&, const variable&, uint64_t);
  void (*config_save_module) (scope&, const char*, int);
  const string& (*config_preprocess_create) (context&,
                                             values&,
                                             vector_view<opspec>&,
                                             bool,
                                             const location&);
  namespace config
  {
    pair<lookup, bool>
    omitted (scope& rs, const variable& var)
    {
      // This is a stripped-down version of the required()'s twisted logic.

      pair<lookup, size_t> org (rs.find_original (var));

      bool n (false); // New flag.
      lookup l (org.first);

      // Treat an inherited value that was set to default as new.
      //
      if (l.defined () && l->extra)
        n = true;

      if (var.overrides != nullptr)
      {
        pair<lookup, size_t> ovr (rs.find_override (var, move (org)));

        if (l != ovr.first) // Overriden?
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

    lookup
    optional (scope& rs, const variable& var)
    {
      save_variable (rs, var);

      auto l (rs[var]);
      return l.defined ()
        ? l
        : lookup (rs.assign (var), var, rs); // NULL.
    }

    bool
    specified (scope& rs, const string& n)
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
        for (auto p (s->vars.find_namespace (vns));
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
      // Pattern-typed in boot() as bool.
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
      // Pattern-typed in boot() as bool.
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
