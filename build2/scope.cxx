// file      : build2/scope.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scope>

#include <build2/target>

using namespace std;

namespace build2
{
  // scope
  //
  lookup<const value> scope::
  lookup (const target_type* tt, const string* name, const variable& var) const
  {
    using result = build2::lookup<const value>;

    for (const scope* s (this); s != nullptr; )
    {
      if (tt != nullptr && !s->target_vars.empty ())
      {
        if (auto l = s->target_vars.lookup (*tt, *name, var))
          return l;
      }

      if (auto r = s->vars.find (var))
        return result (r, &s->vars);

      switch (var.visibility)
      {
      case variable_visibility::scope:
        s = nullptr;
        break;
      case variable_visibility::project:
        s = s->root () ? nullptr : s->parent_scope ();
        break;
      case variable_visibility::normal:
        s = s->parent_scope ();
        break;
      }
    }

    return result ();
  }

  value& scope::
  append (const variable& var)
  {
    auto l (operator[] (var));

    if (l && l.belongs (*this)) // Existing variable in this scope.
      return const_cast<value&> (*l);

    value& r (assign (var));

    if (l)
      r = *l; // Copy value from the outer scope.

    return r;
  }

  const target_type* scope::
  find_target_type (const string& tt, const scope** rs) const
  {
    // Search scopes outwards, stopping at the project root.
    //
    for (const scope* s (this);
         s != nullptr;
         s = s->root () ? global_scope : s->parent_scope ())
    {
      if (s->target_types.empty ())
        continue;

      auto i (s->target_types.find (tt));

      if (i != s->target_types.end ())
      {
        if (rs != nullptr)
          *rs = s;

        return &i->second.get ();
      }
    }

    return nullptr;
  }

  static const string dir_tt ("dir");
  static const string file_tt ("file");

  const target_type* scope::
  find_target_type (name& n, const string*& ext) const
  {
    ext = nullptr;

    string& v (n.value);

    // First determine the target type.
    //
    const string* tt;
    if (n.untyped ())
    {
      // Empty name or '.' and '..' signify a directory.
      //
      if (v.empty () || v == "." || v == "..")
        tt = &dir_tt;
      else
        //@@ TODO: derive type from extension.
        //
        tt = &file_tt;
    }
    else
      tt = &n.type;

    const target_type* r (find_target_type (*tt));

    if (r == nullptr)
      return r;

    // Directories require special name processing. If we find that more
    // targets deviate, then we should make this target-type-specific.
    //
    if (r->is_a<dir> () || r->is_a<fsdir> ())
    {
      // The canonical representation of a directory name is with empty
      // value.
      //
      if (!v.empty ())
      {
        n.dir /= dir_path (v); // Move name value to dir.
        v.clear ();
      }
    }
    else
    {
      // Split the path into its directory part (if any) the name part,
      // and the extension (if any). We cannot assume the name part is
      // a valid filesystem name so we will have to do the splitting
      // manually.
      //
      path::size_type i (path::traits::rfind_separator (v));

      if (i != string::npos)
      {
        n.dir /= dir_path (v, i != 0 ? i : 1); // Special case: "/".
        v = string (v, i + 1, string::npos);
      }

      // Extract the extension.
      //
      string::size_type j (path::traits::find_extension (v));

      if (j != string::npos)
      {
        ext = &extension_pool.find (v.c_str () + j + 1);
        v.resize (j);
      }
    }

    return r;
  }

  // scope_map
  //
  scope_map scopes;
  scope* global_scope;

  auto scope_map::
  insert (const dir_path& k, scope* ns, bool parent, bool root) -> iterator
  {
    auto er (map_.emplace (k, nullptr));
    scope*& ps (er.first->second);

    if (er.second)
      ps = ns == nullptr ? new scope : ns;
    else if (ns != nullptr && ps != ns)
    {
      assert (ps->out_path_ == nullptr || ps->src_path_ == nullptr);

      if (!ps->empty ())
        fail << "attempt to replace non-empty scope " << k;

      // Un-parent ourselves. We will becomes a new parent below,
      // if requested by the caller.
      //
      auto r (map_.find_prefix (k)); // The first entry is ourselves.
      for (++r.first; r.first != r.second; ++r.first)
      {
        scope& c (*r.first->second);

        if (c.parent_ == ps) // No intermediate parent.
          c.parent_ = ps->parent_;
      }

      delete ps;
      ps = ns;
      er.second = true;
    }

    scope& s (*ps);

    if (parent)
    {
      if (er.second)
      {
        scope* p (nullptr);

        // Update scopes of which we are a new parent/root (unless this
        // is the global scope). Also find our parent while at it.
        //
        if (map_.size () > 1)
        {
          // The first entry is ourselves.
          //
          auto r (map_.find_prefix (k));
          for (++r.first; r.first != r.second; ++r.first)
          {
            scope& c (*r.first->second);

            // The child-parent relationship is based on the out hierarchy,
            // thus the extra check.
            //
            if (c.out_path_ != nullptr && !c.out_path_->sub (k))
              continue;

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

        s.parent_ = p;
        s.root_ = root ? &s : (p != nullptr ? p->root_ : nullptr);
      }
      else if (root && !s.root ())
      {
        // Upgrade to root scope.
        //
        auto r (map_.find_prefix (k));
        for (++r.first; r.first != r.second; ++r.first)
        {
          scope& c (*r.first->second);

          if (c.root_ == s.root_) // No intermediate root.
            c.root_ = &s;
        }

        s.root_ = &s;
      }
    }
    else
      assert (s.parent_ != nullptr);

    return er.first;
  }

  // Find the most qualified scope that encompasses this path.
  //
  scope& scope_map::
  find (const dir_path& k) const
  {
    // Normally we would have a scope for the full path so try
    // that before making any copies.
    //
    auto i (map_.find (k)), e (map_.end ());

    if (i != e)
      return *i->second;

    for (dir_path d (k.directory ());; d = d.directory ())
    {
      auto i (map_.find (d));

      if (i != e)
        return *i->second;

      assert (!d.empty ()); // We should have the global scope.
    }
  }

  void scope_map::
  clear ()
  {
    for (auto& p: map_)
    {
      scope* s (p.second);

      if (s->out_path_ == &p.first)
        s->out_path_ = nullptr;

      if (s->src_path_ == &p.first)
        s->src_path_ = nullptr;

      if (s->out_path_ == nullptr && s->src_path_ == nullptr)
        delete s;
    }

    map_.clear ();
  }
}
