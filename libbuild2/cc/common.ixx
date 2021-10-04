// file      : libbuild2/cc/common.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace cc
  {
    inline const scope* data::
    effective_iscope (const scope& bs) const
    {
      if (iscope)
      {
        switch (*iscope)
        {
        case internal_scope::current: return iscope_current;
        case internal_scope::base:    return &bs;
        case internal_scope::root:    return bs.root_scope ();
        case internal_scope::bundle:  return bs.bundle_scope ();
        case internal_scope::strong:  return bs.strong_scope ();
        case internal_scope::weak:    return bs.weak_scope ();
        }
      }

      return nullptr;
    }
  }
}
