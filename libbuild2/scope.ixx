// file      : libbuild2/scope.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  // scope
  //
  inline bool scope::
  root () const
  {
    return root_ == this;
  }

  inline bool scope::
  amalgamatable () const
  {
    return (root_extra == nullptr ||
            !root_extra->amalgamation ||
            *root_extra->amalgamation != nullptr);
  }

  inline scope* scope::
  parent_scope ()
  {
    // If this is a root scope and amalgamation is disabled, "jump" straight
    // to the global scope.
    //
    return root () && !amalgamatable () ? &global_scope () : parent_;
  }

  inline const scope* scope::
  parent_scope () const
  {
    return root () && !amalgamatable () ? &global_scope () : parent_;
  }

  inline scope* scope::
  root_scope ()
  {
    return root_;
  }

  inline const scope* scope::
  root_scope () const
  {
    return root_;
  }

  inline scope* scope::
  strong_scope ()
  {
    // We naturally assume strong_ is not set for non-amalgamatable projects.
    //
    return root_ != nullptr
      ? root_->strong_ != nullptr ? root_->strong_ : root_
      : nullptr;
  }

  inline const scope* scope::
  strong_scope () const
  {
    return root_ != nullptr
      ? root_->strong_ != nullptr ? root_->strong_ : root_
      : nullptr;
  }

  inline scope* scope::
  bundle_scope ()
  {
    if (auto r = root_)
    {
      for (auto s (r), a (strong_scope ()); s != a; )
      {
        s = s->parent_scope ()->root_scope ();

        if (s->root_extra != nullptr           &&
            s->root_extra->project             &&
            *s->root_extra->project != nullptr &&
            !(*s->root_extra->project)->empty ())
          r = s; // Last named.
      }

      return r;
    }

    return nullptr;
  }

  inline const scope* scope::
  bundle_scope () const
  {
    if (const auto* r = root_)
    {
      for (auto s (r), a (strong_scope ()); s != a; )
      {
        s = s->parent_scope ()->root_scope ();

        if (s->root_extra != nullptr           &&
            s->root_extra->project             &&
            *s->root_extra->project != nullptr &&
            !(*s->root_extra->project)->empty ())
          r = s; // Last named.
      }

      return r;
    }

    return nullptr;
  }

  inline scope* scope::
  weak_scope ()
  {
    scope* r (root_);
    if (r != nullptr)
      for (;
           r->amalgamatable () && r->parent_->root_ != nullptr;
           r = r->parent_->root_) ;
    return r;
  }

  inline const scope* scope::
  weak_scope () const
  {
    const scope* r (root_);
    if (r != nullptr)
      for (;
           r->amalgamatable () && r->parent_->root_ != nullptr;
           r = r->parent_->root_) ;
    return r;
  }

  inline bool scope::
  sub_root (const scope& r) const
  {
    // Scan the parent root scope chain looking for this scope.
    //
    for (const scope* pr (&r);
         pr->amalgamatable () && (pr = pr->parent_->root_) != nullptr; )
    {
      if (pr == this)
        return true;
    }

    return false;
  }

  inline target_key scope::
  find_target_key (name& n, name& o,
                   const location& loc,
                   const target_type* tt) const
  {
    auto p (find_target_type (n, o, loc, tt));
    return target_key {
      &p.first,
      &n.dir,
      o.dir.empty () ? &empty_dir_path : &o.dir,
      &n.value,
      move (p.second)};
  }

  inline prerequisite_key scope::
  find_prerequisite_key (name& n, name& o,
                         const location& loc,
                         const target_type* tt) const
  {
    auto p (find_prerequisite_type (n, o, loc, tt));
    return prerequisite_key {
      n.proj,
      {
        &p.first,
        &n.dir,
        o.dir.empty () ? &empty_dir_path : &o.dir,
        &n.value,
        move (p.second)
      },
      this};
  }

  template <typename T>
  inline void scope::
  insert_rule (meta_operation_id mid, operation_id oid,
               string name,
               const rule& r)
  {
    if (mid != 0)
      rules.insert<T> (mid, oid, move (name), r);
    else
    {
      auto& ms (root_scope ()->root_extra->meta_operations);

      for (size_t i (1), n (ms.size ()); i != n; ++i)
      {
        if (ms[i] != nullptr)
        {
          // Skip a few well-known meta-operations that cannot possibly
          // trigger a rule match.
          //
          mid = static_cast<meta_operation_id> (i);

          if (mid != noop_id     &&
              mid != info_id     &&
              mid != create_id   &&
              mid != disfigure_id)
            rules.insert<T> (mid, oid, name, r);
        }
      }
    }
  }

  inline dir_path
  src_out (const dir_path& out, const scope& r)
  {
    assert (r.root ());
    return src_out (out, r.out_path (), r.src_path ());
  }

  inline dir_path
  out_src (const dir_path& src, const scope& r)
  {
    assert (r.root ());
    return out_src (src, r.out_path (), r.src_path ());
  }

  inline dir_path
  src_out (const dir_path& o,
           const dir_path& out_root, const dir_path& src_root)
  {
    assert (o.sub (out_root));
    return src_root / o.leaf (out_root);
  }

  inline dir_path
  out_src (const dir_path& s,
           const dir_path& out_root, const dir_path& src_root)
  {
    assert (s.sub (src_root));
    return out_root / s.leaf (src_root);
  }

  inline const project_name&
  project (const scope& rs)
  {
    assert (rs.root_extra != nullptr && rs.root_extra->project);

    return *rs.root_extra->project != nullptr
      ? **rs.root_extra->project
      : empty_project_name;
  }

  inline const project_name&
  named_project (const scope& rs)
  {
    for (auto r (&rs), a (rs.strong_scope ());
         ;
         r = r->parent_scope ()->root_scope ())
    {
      const project_name& n (project (*r));
      if (!n.empty ())
        return n;

      if (r == a)
        break;
    }

    return empty_project_name;
  }
}
