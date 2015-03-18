// file      : build/scope.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/scope>

using namespace std;

namespace build
{
  // scope
  //
  value_proxy scope::
  operator[] (const string& name)
  {
    const variable& var (variable_pool.find (name));
    return operator[] (var);
  }

  value_proxy scope::
  operator[] (const variable& var)
  {
    for (scope* s (this); s != nullptr; s = s->parent ())
    {
      auto i (s->variables.find (var));
      if (i != s->variables.end ())
        return value_proxy (&i->second, s);
    }

    return value_proxy (nullptr, nullptr);
  }

  // scope_map
  //
  scope_map scopes;
  scope* root_scope;

  pair<scope&, bool> scope_map::
  insert (const path& k)
  {
    auto er (emplace (k, scope ()));
    scope& s (er.first->second);

    if (er.second)
    {
      scope* p (nullptr);

      // Update scopes of which we are a new parent (unless this is the
      // root scope).
      //
      if (size () > 1)
      {
        // The first entry is ourselves.
        //
        auto r (find_prefix (k));
        for (++r.first; r.first != r.second; ++r.first)
        {
          scope& c (r.first->second);

          // The first scope of which we are a parent is the least
          // (shortest) one which means there is no other scope
          // between it and our parent.
          //
          if (p == nullptr)
            p = c.parent ();
          else if (p != c.parent ()) // A scope with an intermediate parent.
            continue;

          c.parent (s);
        }

        // We couldn't get the parent from one of its old children
        // so we have to find it ourselves.
        //
        if (p == nullptr)
          p = &find (k.directory ());
      }

      s.init (er.first, p);
    }

    return pair<scope&, bool> (s, er.second);
  }

  // Find the most qualified scope that encompasses this path.
  //
  scope& scope_map::
  find (const path& k)
  {
    // Normally we would have a scope for the full path so try
    // that before making any copies.
    //
    auto i (base::find (k));

    if (i != end ())
      return i->second;

    for (path d (k.directory ());; d = d.directory ())
    {
      auto i (base::find (d));

      if (i != end ())
        return i->second;

      assert (!d.empty ()); // We should have the root scope.
    }
  }
}
