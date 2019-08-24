// file      : libbuild2/cc/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_UTILITY_HXX
#define LIBBUILD2_CC_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>
#include <libbuild2/bin/target.hxx>

#include <libbuild2/cc/types.hxx>

namespace build2
{
  struct variable;

  namespace cc
  {
    // To form the complete path do:
    //
    //   root.out_path () / root.root_extra->build_dir / module_dir
    //
    extern const dir_path module_dir;             // cc/
    extern const dir_path modules_sidebuild_dir;  // cc/modules/

    // Compile output type.
    //
    otype
    compile_type (const target&, unit_type);

    compile_target_types
    compile_types (otype);

    // Link output type.
    //
    ltype
    link_type (const target&);

    // Library link order.
    //
    // The reason we pass scope and not the target is because this function is
    // called not only for exe/lib but also for obj as part of the library
    // meta-information protocol implementation. Normally the bin.*.lib values
    // will be project-wide. With this scheme they can be customized on the
    // per-directory basis but not per-target which means all exe/lib in the
    // same directory have to have the same link order.
    //
    lorder
    link_order (const scope& base, otype);

    inline linfo
    link_info (const scope& base, otype ot)
    {
      return linfo {ot, link_order (base, ot)};
    }

    // Given the link order return the library member to link. That is, liba{}
    // or libs{} for lib{} and libua{} or libus{} for libul{}.
    //
    // If existing is true, then only return the member target if it exists
    // (currently only used and supported for utility libraries).
    //
    const target*
    link_member (const bin::libx&, action, linfo, bool existing = false);
  }
}

#include <libbuild2/cc/utility.ixx>

#endif // LIBBUILD2_CC_UTILITY_HXX
