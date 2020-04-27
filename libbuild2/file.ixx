// file      : libbuild2/file.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/export.hxx>

namespace build2
{
  inline bool
  source_once (scope& root, scope& base, const path& bf)
  {
    return source_once (root, base, bf, base);
  }

  inline pair<name, optional<dir_path>>
  import_search (scope& base,
                 name tgt,
                 bool opt, const optional<string>& md, bool sp,
                 const location& loc, const char* w)
  {
    bool dummy (false);
    return import_search (dummy, base, move (tgt), opt, md, sp, loc, w);
  }

  LIBBUILD2_SYMEXPORT const target*
  import (context&,
          const prerequisite_key&,
          bool optional_,
          const optional<string>& metadata, // False or metadata key.
          bool existing,
          const location&);

  inline const target&
  import (context& ctx, const prerequisite_key& pk)
  {
    assert (ctx.phase == run_phase::match);

    // @@ We no longer have location. This is especially bad for the empty
    //    project case (i.e., where do I need to specify the project name)?
    //    Looks like the only way to do this is to keep location in name and
    //    then in prerequisite. Perhaps one day...
    //
    return *import (ctx, pk, false, nullopt, false, location ());
  }

  inline pair<const target*, import_kind>
  import_direct (scope& base,
                 name tgt,
                 bool ph2, bool opt, bool md,
                 const location& loc, const char* w)
  {
    bool dummy (false);
    return import_direct (dummy, base, move (tgt), ph2, opt, md, loc, w);
  }

  template <typename T>
  inline pair<const T*, import_kind>
  import_direct (scope& base,
                 name tgt,
                 bool ph2, bool opt, bool md,
                 const location& loc, const char* w)
  {
    auto r (import_direct (base, move (tgt), ph2, opt, md, loc, w));
    return make_pair (r.first != nullptr ? &r.first->as<const T> () : nullptr,
                      r.second);
  }

  template <typename T>
  inline pair<const T*, import_kind>
  import_direct (bool& nv,
                 scope& base,
                 name tgt,
                 bool ph2, bool opt, bool md,
                 const location& loc, const char* w)
  {
    auto r (import_direct (nv, base, move (tgt), ph2, opt, md, loc, w));
    return make_pair (r.first != nullptr ? &r.first->as<const T> () : nullptr,
                      r.second);
  }

  inline const target*
  import_existing (context& ctx, const prerequisite_key& pk)
  {
    assert (ctx.phase == run_phase::match || ctx.phase == run_phase::execute);
    return import (ctx, pk, false, nullopt, true, location ());
  }
}
