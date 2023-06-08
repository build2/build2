// file      : libbuild2/file.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/export.hxx>

namespace build2
{
  inline bool
  source_once (scope& root, scope& base, const path& bf)
  {
    return source_once (root, base, bf, root);
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
  import2 (context&,
           const prerequisite_key&,
           const string& hint,
           bool optional_,
           const optional<string>& metadata, // False or metadata key.
           bool existing,
           const location&);

  inline const target&
  import2 (context& ctx, const prerequisite_key& pk)
  {
    assert (ctx.phase == run_phase::match);

    // @@ We no longer have location. This is especially bad for the empty
    //    project case (i.e., where do I need to specify the project name)?
    //    Looks like the only way to do this is to keep location in name and
    //    then in prerequisite. Perhaps one day...
    //
    return *import2 (ctx, pk, string (), false, nullopt, false, location ());
  }

  inline import_result<target>
  import_direct (scope& base,
                 name tgt,
                 const optional<string>& ph2, bool opt, bool md,
                 const location& loc, const char* w)
  {
    bool dummy (false);
    return import_direct (dummy, base, move (tgt), ph2, opt, md, loc, w);
  }

  template <typename T>
  inline import_result<T>
  import_direct (scope& base,
                 name tgt,
                 bool ph2, bool opt, bool md,
                 const location& loc, const char* w)
  {
    auto r (import_direct (base,
                           move (tgt),
                           ph2 ? optional<string> (string ()) : nullopt,
                           opt,
                           md,
                           loc,
                           w));
    return import_result<T> {
      r.target != nullptr ? &r.target->as<const T> () : nullptr,
      move (r.name),
      r.kind};
  }

  template <typename T>
  inline import_result<T>
  import_direct (bool& nv,
                 scope& base,
                 name tgt,
                 bool ph2, bool opt, bool md,
                 const location& loc, const char* w)
  {
    auto r (import_direct (nv,
                           base,
                           move (tgt),
                           ph2 ? optional<string> (string ()) : nullopt,
                           opt,
                           md,
                           loc,
                           w));
    return import_result<T> {
      r.target != nullptr ? &r.target->as<const T> () : nullptr,
      move (r.name),
      r.kind};
  }

  inline const target*
  import_existing (context& ctx, const prerequisite_key& pk)
  {
    return import2 (ctx, pk, string (), false, nullopt, true, location ());
  }
}
