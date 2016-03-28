// file      : build2/config/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/config/utility>

#include <build2/context>

using namespace std;

namespace build2
{
  namespace config
  {
    const value&
    optional (scope& root, const variable& var)
    {
      auto l (root[var]);

      return l.defined ()
        ? l.belongs (*global_scope) ? (root.assign (var) = *l) : *l
        : root.assign (var); // NULL
    }

    const value&
    optional_absolute (scope& root, const variable& var)
    {
      auto l (root[var]);

      if (!l.defined ())
        return root.assign (var); // NULL

      if (!l.belongs (*global_scope)) // Value from (some) root scope.
        return *l;

      // Make the command-line value absolute. This is necessary to avoid
      // a warning issued by the config module about global/root scope
      // value mismatch.
      //
      // @@ CMDVAR
      //
      value& v (const_cast<value&> (*l));

      if (v && !v.empty ())
      {
        dir_path& d (cast<dir_path> (v));

        if (d.relative ())
        {
          d = work / d;
          d.normalize ();
        }
      }

      return root.assign (var) = v;
    }

    bool
    specified (scope& r, const string& ns)
    {
      // Search all outer scopes for any value in this namespace.
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
  }
}
