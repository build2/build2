// file      : build/scope.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/scope>

using namespace std;

namespace build
{
  // scope
  //
  value* scope::
  operator[] (const string& name)
  {
    const variable& var (variable_pool.find (name));
    return (*this)[var];
  }

  value* scope::
  operator[] (const variable& var)
  {
    for (scope* s (this); s != nullptr; s = s->parent ())
    {
      auto i (s->variables.find (var));
      if (i != s->variables.end ())
        return i->second.get ();
    }

    return nullptr;
  }

  // scope_map
  //
  scope_map scopes;
  scope* root_scope;

  scope& scope_map::
  operator[] (const path& k)
  {
    auto er (emplace (k, scope ()));
    scope& s (er.first->second);

    if (er.second)
    {
      scope* p (nullptr);

      // Update scopes of which we are a new parent.
      //
      for (auto r (find_prefix (k)); r.first != r.second; ++r.first)
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
      // so we have to find it ourselves (unless this is is the
      // root scope).
      //
      if (p == nullptr && size () != 1)
        p = &find (k.directory ());

      s.init (er.first, p);
    }

    return s;
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
