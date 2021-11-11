// file      : libbuild2/cc/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_UTILITY_HXX
#define LIBBUILD2_CC_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>
#include <libbuild2/filesystem.hxx>

#include <libbuild2/bin/target.hxx>
#include <libbuild2/bin/utility.hxx>

#include <libbuild2/cc/types.hxx>

namespace build2
{
  namespace cc
  {
    using bin::link_type;
    using bin::link_order;
    using bin::link_info;
    using bin::link_member;

    // To form the complete path do:
    //
    //   root.out_path () / root.root_extra->build_dir / X_dir
    //
    extern const dir_path module_dir;               // cc/
    extern const dir_path module_build_dir;         // cc/build/
    extern const dir_path module_build_modules_dir; // cc/build/modules/

    // Compile output type from output target type (obj*{}, bmi*{}, etc).
    //
    // If input unit type is specified, then restrict the tests only to output
    // types that can be produced from this input.
    //
    otype
    compile_type (const target_type&, optional<unit_type> = nullopt);

    inline otype
    compile_type (const target& t, optional<unit_type> ut = nullopt)
    {
      return compile_type (t.type (), ut);
    }

    compile_target_types
    compile_types (otype);

    // Normalize an absolute path to an existing header.
    //
    inline void
    normalize_header (path& f)
    {
      normalize_external (f, "header");
    }
  }
}

#include <libbuild2/cc/utility.ixx>

#endif // LIBBUILD2_CC_UTILITY_HXX
