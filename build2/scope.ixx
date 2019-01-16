// file      : build2/scope.ixx -*- C++ -*-
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
}
