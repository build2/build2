// file      : libbuild2/cc/common.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace cc
  {
    inline const scope* data::
    effective_internal_scope (const scope& bs) const
    {
      if (internal_scope == nullptr)
        return nullptr;
      else
      {
        const string& s (*internal_scope);

        if (s == "current")
          return internal_scope_current;
        else if (s == "base")
          return &bs;
        else if (s == "root")
          return bs.root_scope ();
        else if (s == "bundle")
          return bs.bundle_scope ();
        else if (s == "strong")
          return bs.strong_scope ();
        else if (s == "weak")
          return bs.weak_scope ();
        else
          return nullptr;
      }
    }
  }
}
