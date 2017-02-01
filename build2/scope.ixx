// file      : build2/scope.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
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
}
