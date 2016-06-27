// file      : build2/config/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/config/utility>

#include <build2/context>

#include <build2/config/module>

using namespace std;

namespace build2
{
  namespace config
  {
    void
    save_variable (scope& r, const variable& var, uint64_t flags)
    {
      if (current_mif->id == configure_id)
      {
        // The project might not be using the config module. But then how
        // could we be configuring it? Good question.
        //
        if (module* mod = r.modules.lookup<module> (module::name))
          mod->vars.emplace (var, flags);
      }
    }

    pair<const value*, bool>
    required (scope& r, const variable& var)
    {
      // This is a stripped-down version of the other required() twisted
      // implementation.

      pair<lookup, size_t> org (r.find_original (var));

      bool n (false); // New flag.
      lookup l (org.first);

      // Treat an inherited value that was set to default as new.
      //
      if (l.defined () && l->extra)
        n = true;

      if (var.override != nullptr)
      {
        pair<lookup, size_t> ovr (r.find_override (var, move (org)));

        if (l != ovr.first) // Overriden?
        {
          // Override is always treated as new.
          //
          n = true;
          l = move (ovr.first);
        }
      }

      if (l.defined () && current_mif->id == configure_id)
          save_variable (r, var);

      return pair<const value*, bool> (l.value, n);
    }

    const value&
    optional (scope& r, const variable& var)
    {
      if (current_mif->id == configure_id)
        save_variable (r, var);

      auto l (r[var]);
      return l.defined ()
        ? *l
        : r.assign (var); // NULL.
    }

    bool
    specified (scope& r, const string& ns)
    {
      // Search all outer scopes for any value in this namespace.
      //
      // What about "pure" overrides, i.e., those without any original values?
      // Well, they will also be found since their names have the original
      // variable as a prefix. But do they apply? Yes, since we haven't found
      // any original values, they will be "visible"; see find_override() for
      // details.
      //
      for (scope* s (&r); s != nullptr; s = s->parent_scope ())
      {
        for (auto p (s->vars.find_namespace (ns));
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
    unconfigured (scope& root, const string& ns)
    {
      // Note: not overridable.
      //
      const variable& var (var_pool.insert<bool> (ns + ".configured"));

      if (current_mif->id == configure_id)
        save_variable (root, var);

      auto l (root[var]); // Include inherited values.
      return l && !cast<bool> (l);
    }

    void
    unconfigured (scope& root, const string& ns, bool v)
    {
      // Note: not overridable.
      //
      const variable& var (var_pool.insert<bool> (ns + ".configured"));

      if (current_mif->id == configure_id)
        save_variable (root, var);

      root.assign (var) = !v;
    }
  }
}
