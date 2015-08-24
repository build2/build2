// file      : build/config/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/config/utility>

using namespace std;

namespace build
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

    bool
    specified (scope& r, const string& ns)
    {
      // Search all outer scopes for any value in this namespace.
      //
      for (scope* s (&r); s != nullptr; s = s->parent_scope ())
      {
        auto p (s->vars.find_namespace (ns));
        if (p.first != p.second)
          return true;
      }

      return false;
    }

    void
    append_options (cstrings& args, const const_strings_value& sv)
    {
      if (!sv.empty ())
      {
        args.reserve (args.size () + sv.size ());

        for (const string& s: sv)
          args.push_back (s.c_str ());
      }
    }
  }
}
