// file      : build/scope.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/scope>

using namespace std;

namespace build
{
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
          p = &c;
        else if (p != &c) // A scope with an intermediate parent.
          continue;

        c.parent (s);
      }

      // We couldn't get the parent from one of its old children
      // so we have to find it ourselves (unless this is is the
      // root scope).
      //
      if (p == nullptr && size () != 1)
        p = &find (k);

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
      auto i (base::find (k));

      if (i != end ())
        return i->second;

      assert (d.empty ()); // We should have the root scope.
    }
  }
}
