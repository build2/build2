// file      : build/scope.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/scope>

using namespace std;

namespace build
{
  // scope
  //
  value_proxy scope::
  operator[] (const variable& var) const
  {
    for (const scope* s (this); s != nullptr; s = s->parent_scope ())
    {
      auto i (s->vars.find (var));
      if (i != s->vars.end ())
        // @@ Same issue as in variable_map: need ro_value_proxy.
        return value_proxy (&const_cast<value_ptr&> (i->second), &s->vars);
    }

    return value_proxy (nullptr, nullptr);
  }

  value_proxy scope::
  append (const variable& var)
  {
    value_proxy val (operator[] (var));

    if (val && val.belongs (*this)) // Existing variable in this scope.
      return val;

    value_proxy r (assign (var));

    if (val)
      r = val; // Copy value from the outer scope.

    return r;
  }

  // scope_map
  //
  scope_map scopes;
  scope* global_scope;

  pair<scope&, bool> scope_map::
  insert (const dir_path& k, bool root)
  {
    auto er (emplace (k, scope ()));
    scope& s (er.first->second);

    if (er.second)
    {
      scope* p (nullptr);

      // Update scopes of which we are a new parent/root (unless this
      // is the global scope).
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
            p = c.parent_;

          if (root && c.root_ == p->root_) // No intermediate root.
            c.root_ = &s;

          if (p == c.parent_) // No intermediate parent.
            c.parent_ = &s;
        }

        // We couldn't get the parent from one of its old children
        // so we have to find it ourselves.
        //
        if (p == nullptr)
          p = &find (k.directory ());
      }

      s.i_ = er.first;
      s.parent_ = p;
      s.root_ = root ? &s : (p != nullptr ? p->root_ : nullptr);
    }
    else if (root && !s.root ())
    {
      // Upgrade to root scope.
      //
      auto r (find_prefix (k));
      for (++r.first; r.first != r.second; ++r.first)
      {
        scope& c (r.first->second);

        if (c.root_ == s.root_) // No intermediate root.
          c.root_ = &s;
      }

      s.root_ = &s;
    }

    return pair<scope&, bool> (s, er.second);
  }

  // Find the most qualified scope that encompasses this path.
  //
  scope& scope_map::
  find (const dir_path& k)
  {
    // Normally we would have a scope for the full path so try
    // that before making any copies.
    //
    auto i (base::find (k));

    if (i != end ())
      return i->second;

    for (dir_path d (k.directory ());; d = d.directory ())
    {
      auto i (base::find (d));

      if (i != end ())
        return i->second;

      assert (!d.empty ()); // We should have the global scope.
    }
  }
}
