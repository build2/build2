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
  pair<lookup, size_t> scope::
  find_original (const variable& var,
                 const target_type* tt, const string* tn,
                 const target_type* gt, const string* gn) const
  {
    size_t d (0);

    for (const scope* s (this); s != nullptr; )
    {
      if (tt != nullptr) // This started from the target.
      {
        bool f (!s->target_vars.empty ());

        ++d;
        if (f)
          if (auto l = s->target_vars.find (*tt, *tn, var))
            return make_pair (move (l), d);

        ++d;
        if (f && gt != nullptr)
          if (auto l = s->target_vars.find (*gt, *gn, var))
            return make_pair (move (l), d);
      }

      ++d;
      if (auto r = s->vars.find (var))
        return make_pair (lookup (r, &s->vars), d);

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

    return make_pair (lookup (), size_t (~0));
  }

  pair<lookup, size_t> scope::
  find_override (const variable& var,
                 pair<lookup, size_t>&& original,
                 bool target) const
  {
    // Normally there would be no overrides and if there are, there will only
    // be a few of them. As a result, here we concentrate on keeping the logic
    // as straightforward as possible without trying to optimize anything.
    //
    // Note also that we rely (e.g., in the config module) on the fact that if
    // no overrides apply, then we return the original value and not its copy
    // in the cache (this can be used to detect if the value was overriden).
    //
    //
    assert (var.override != nullptr);

    lookup& origl (original.first);
    size_t origd (original.second);

    // The first step is to find out where our cache will reside. After some
    // meditation it becomes clear it should be next to the innermost (scope-
    // wise) value (override or original) that could contribute to the end
    // result.
    //
    const variable_map* vars (nullptr);

    // Root scope of a project from which our initial value comes. See below.
    //
    const scope* proj (nullptr);

    // One special case is if the original is target-specific, which is the
    // most innermost. Or is it innermostest?
    //
    bool targetspec (false);
    if (target)
    {
      targetspec = origl.defined () && (origd == 1 || origd == 2);

      if (targetspec)
      {
        vars = origl.vars;
        proj = root_scope ();
      }
    }

    const scope* s;

    // Return true if the override applies. Note that it expects vars and proj
    // to be not NULL; if there is nothing "inner", then any override will
    // still be "visible".
    //
    auto applies = [&vars, &s] (const variable* o, const scope* proj) -> bool
    {
      switch (o->visibility)
      {
      case variable_visibility::scope:
      {
        // Does not apply if the innermost value is not in this scope.
        //
        if (vars != &s->vars)
          return false;

        break;
      }
      case variable_visibility::project:
      {
        // Does not apply if in a different project.
        //
        if (proj != s->root_scope ())
          return false;

        break;
      }
      case variable_visibility::normal:
        break;
      }

      return true;
    };

    // Return override value if it is present and optionally ends with suffix.
    //
    auto find = [&s] (const variable* o, const char* sf = nullptr) -> lookup
    {
      if (sf != nullptr && o->name.rfind (sf) == string::npos)
        return lookup ();

      return lookup (s->vars.find (*o), &s->vars);
    };

    // Return true if a value is from this scope (either target type/pattern-
    // specific or ordinary).
    //
    auto belongs = [&s] (const lookup& l) -> bool
    {
      for (auto& p1: s->target_vars)
        for (auto& p2: p1.second)
          if (l.vars == &p2.second)
            return true;

      return l.vars == &s->vars;
    };

    // While looking for the cache we can also detect if none of the overrides
    // apply. In this case the result is simply the original value (if any).
    //
    bool apply (false);

    for (s = this; s != nullptr; s = s->parent_scope ())
    {
      // If we are still looking for the cache, see if the original comes from
      // this scope. We check this before the overrides since it can come from
      // the target type/patter-specific variables, which is "more inner" than
      // normal scope variables (see find_original()).
      //
      if (vars == nullptr && origl.defined () && belongs (origl))
      {
        vars = origl.vars;
        proj = s->root_scope (); // This is so we skip non-recursive overrides
                                 // that would not apply. We reset it later.
      }

      for (const variable* o (var.override.get ());
           o != nullptr;
           o = o->override.get ())
      {
        if (vars != 0 && !applies (o, proj))
          continue;

        auto l (find (o));

        if (l.defined ())
        {
          if (vars == nullptr)
            vars = l.vars;

          apply = true;
          break;
        }
      }

      // If we found the cache and at least one override applies, then we can
      // stop.
      //
      if (vars != nullptr && apply)
        break;
    }

    if (!apply)
      return move (original);

    assert (vars != nullptr);

    // Implementing proper caching is tricky so for now we are going to re-
    // calculate the value every time. Later, the plan is to use value
    // versioning (incremented on every update) to detect stem value changes.
    // We also need to watch out for the change of the stem itself in addition
    // to its value (think of a new variable set since last lookup which is
    // now a new stem). Thus stem_vars in variable_override_value.
    //
    // @@ MT
    //
    variable_override_value& cache (
      variable_override_cache[make_pair (vars, &var)]);

    // Now find our "stem", that is the value to which we will be appending
    // suffixes and prepending prefixes. This is either the original or the
    // __override provided it applies. We may also not have either.
    //
    lookup stem (targetspec ? origl : lookup ());
    size_t depth (targetspec ? origd : 0);
    size_t ovrd (target ? 2 : 0); // For implied target-specific lookup.

    for (s = this; s != nullptr; s = s->parent_scope ())
    {
      bool done (false);

      // First check if the original is from this scope.
      //
      if (origl.defined () && belongs (origl))
      {
        stem = origl;
        depth = origd;
        proj = s->root_scope ();
        // Keep searching.
      }

      ++ovrd;

      // Then look for an __override that applies.
      //
      for (const variable* o (var.override.get ());
           o != nullptr;
           o = o->override.get ())
      {
        // If we haven't yet found anything, then any override will still be
        // "visible" even if it doesn't apply.
        //
        if (stem.defined () && !applies (o, root_scope ()))
          continue;

        auto l (find (o, ".__override"));

        if (l.defined ())
        {
          depth = ovrd;
          stem = move (l);
          proj = s->root_scope ();
          done = true;
          break;
        }
      }

      if (done)
        break;
    }

    // If there is a stem, set it as the initial value of the cache.
    // Otherwise, start with a NULL value.
    //

    // Un-typify the cache. This can be necessary, for example, if we are
    // changing from one value-typed stem to another.
    //
    if (!stem.defined () || cache.value.type != stem->type)
    {
      cache.value = nullptr;
      cache.value.type = nullptr; // Un-typify.
    }

    if (stem.defined ())
    {
      cache.value = *stem;
      cache.stem_vars = stem.vars;
    }
    else
      cache.stem_vars = nullptr; // No stem.

    // Typify the cache value. If the stem is the original, then the type
    // would get propagated automatically. But the stem could also be the
    // override, which is kept untyped. Or the stem might not be there at all
    // while we still need to apply prefixes/suffixes in the type-aware way.
    //
    if (cache.value.type == nullptr && var.type != nullptr)
      typify (cache.value, *var.type, var);

    // Now apply override prefixes and suffixes.
    //
    ovrd = target ? 2 : 0;
    const variable_map* ovrv (cache.stem_vars);

    for (s = this; s != nullptr; s = s->parent_scope ())
    {
      ++ovrd;

      for (const variable* o (var.override.get ());
           o != nullptr;
           o = o->override.get ())
      {
        // First see if this override applies. This is actually tricky: what
        // if the stem is a "visible" override from an outer project?
        // Shouldn't its overrides apply? Sure sounds logical. So it seems
        // we should use the project of the stem's scope and not the project
        // of this scope.
        //
        if (proj != nullptr && !applies (o, proj))
          continue;

        // Note that we keep override values as untyped names even if the
        // variable itself is typed. We also pass the original variable for
        // diagnostics.
        //
        auto l (find (o, ".__prefix"));

        if (l) // No sense to prepend/append if NULL.
        {
          cache.value.prepend (names (cast<names> (l)), var);
        }
        else if ((l = find (o, ".__suffix")))
        {
          cache.value.append (names (cast<names> (l)), var);
        }

        if (l.defined ())
        {
          // If we had no stem, use the scope of the first override that
          // applies as the project. For vars/depth we need to pick the
          // innermost.
          //
          if (proj == nullptr)
          {
            proj = s->root_scope ();
            depth = ovrd;
            ovrv = &s->vars;
          }
          else if (ovrd < depth)
          {
            depth = ovrd;
            ovrv = &s->vars;
          }
        }
      }
    }

    // Use the location of the innermost value that contributed as the
    // location of the result.
    //
    return make_pair (lookup (&cache.value, ovrv), depth);
  }

  value& scope::
  append (const variable& var)
  {
    // Note that here we want the original value without any overrides
    // applied.
    //
    lookup l (find_original (var).first);

    if (l.defined () && l.belongs (*this)) // Existing var in this scope.
      return const_cast<value&> (*l); // Ok since this is original.

    value& r (assign (var)); // NULL.

    if (l.defined ())
      r = *l; // Copy value (and type) from the outer scope.

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
