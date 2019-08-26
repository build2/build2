// file      : libbuild2/scope.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  // scope
  //
  inline scope* scope::
  strong_scope ()
  {
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
  weak_scope ()
  {
    scope* r (root_);
    if (r != nullptr)
      for (; r->parent_->root_ != nullptr; r = r->parent_->root_) ;
    return r;
  }

  inline const scope* scope::
  weak_scope () const
  {
    const scope* r (root_);
    if (r != nullptr)
      for (; r->parent_->root_ != nullptr; r = r->parent_->root_) ;
    return r;
  }

  inline bool scope::
  sub_root (const scope& r) const
  {
    // Scan the parent root scope chain looking for this scope.
    //
    for (const scope* pr (&r); (pr = pr->parent_->root_) != nullptr; )
      if (pr == this)
        return true;

    return false;
  }

  inline target_key scope::
  find_target_key (name& n, name& o, const location& loc) const
  {
    auto p (find_target_type (n, o, loc));
    return target_key {&p.first, &n.dir, &o.dir, &n.value, move (p.second)};
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
    auto l (rs[rs.ctx.var_project]);
    return l ? cast<project_name> (l) : empty_project_name;
  }
}
