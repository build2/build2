// file      : libbuild2/bin/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_UTILITY_HXX
#define LIBBUILD2_BIN_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>

#include <libbuild2/bin/types.hxx>
#include <libbuild2/bin/target.hxx>

namespace build2
{
  namespace bin
  {
    // @@ Here we conflate the term "link" to mean both linker output and
    //    linking of a library.

    // Linker output type from a target (exe{}, lib*{}).
    //
    ltype
    link_type (const target_type&);

    inline ltype
    link_type (const target& t)
    {
      return link_type (t.type ());
    }

    // Library group (lib{}) members to build according to the bin.lib value.
    //
    LIBBUILD2_BIN_SYMEXPORT lmembers
    link_members (const scope& rs);

    // Library link order.
    //
    // The reason we pass scope and not the target is because this function is
    // called not only for exe/lib but also for obj as part of the library
    // metadata protocol implementation. Normally the bin.*.lib values will be
    // project-wide. With this scheme they can be customized on the per-
    // directory basis but not per-target which means all exe/lib in the same
    // directory have to have the same link order.
    //
    LIBBUILD2_BIN_SYMEXPORT lorder
    link_order (const scope& bs, otype);

    inline linfo
    link_info (const scope& bs, otype ot)
    {
      return linfo {ot, link_order (bs, ot)};
    }

    // Given the link order return the library member to link. That is, liba{}
    // or libs{} for lib{} and libua{} or libus{} for libul{}.
    //
    // If existing is true, then only return the member target if it exists
    // (currently only used and supported for utility libraries).
    //
    LIBBUILD2_BIN_SYMEXPORT const file*
    link_member (const libx&, action, linfo, bool existing = false);

    // As above but return otype::a or otype::s as well as an indication if
    // the member is available.
    //
    // @@ TODO: support utility libraries (see above version).
    //
    pair<otype, bool>
    link_member (lmembers, linfo);

    // Lookup the bin.pattern value and split it into the pattern and the
    // search paths.
    //
    struct pattern_paths
    {
      const char* pattern = nullptr;
      const char* paths   = nullptr;
    };

    LIBBUILD2_BIN_SYMEXPORT pattern_paths
    lookup_pattern (const scope& rs);
  }
}

#include <libbuild2/bin/utility.ixx>

#endif // LIBBUILD2_BIN_UTILITY_HXX
