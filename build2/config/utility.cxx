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
    pair<const value*, bool>
    omitted (scope& r, const variable& var)
    {
      // This is a stripped-down version of the required() twisted
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

    bool
    unconfigured (scope& root, const string& ns, bool v)
    {
      // Note: not overridable.
      //
      const variable& var (var_pool.insert<bool> (ns + ".configured"));

      if (current_mif->id == configure_id)
        save_variable (root, var);

      value& x (root.assign (var));

      if (x.null || cast<bool> (x) != !v)
      {
        x = !v;
        return true;
      }
      else
        return false;
    }

    void
    save_variable (scope& r, const variable& var, uint64_t flags)
    {
      if (current_mif->id != configure_id)
        return;

      // The project might not be using the config module. But then how
      // could we be configuring it? Good question.
      //
      if (module* m = r.modules.lookup<module> (module::name))
      {
        const string& n (var.name);

        // First try to find the module with the name that is the longest
        // prefix of this variable name.
        //
        saved_modules& sm (m->saved_modules);
        auto i (sm.end ());

        if (!sm.empty ())
        {
          i = sm.upper_bound (n);

          // Get the greatest less than, if any. We might still not be a
          // suffix. And we still have to check the last element if
          // upper_bound() returned end().
          //
          if (i == sm.begin () || !sm.key_comp ().prefix ((--i)->first, n))
            i = sm.end ();
        }

        // If no module matched, then create one based on the variable name.
        //
        if (i == sm.end ())
        {
          // @@ For now with 'config.' prefix.
          //
          i = sm.insert (string (n, 0, n.find ('.', 7)));
        }

        // Don't insert duplicates. The config.import vars are particularly
        // susceptible to duplication.
        //
        saved_variables& sv (i->second);
        auto j (sv.find (var));

        if (j == sv.end ())
          sv.push_back (saved_variable {var, flags});
        else
          assert (j->flags == flags);
      }
    }

    void
    save_module (scope& r, const char* name, int prio)
    {
      if (current_mif->id != configure_id)
        return;

      if (module* m = r.modules.lookup<module> (module::name))
        m->saved_modules.insert (string ("config.") += name, prio);
    }
  }
}
