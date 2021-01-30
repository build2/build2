// file      : libbuild2/cc/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_UTILITY_HXX
#define LIBBUILD2_CC_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

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
    // We used to just normalize the path but that could result in an invalid
    // path (e.g., for some system/compiler headers on CentOS 7 with Clang
    // 3.4) because of the symlinks (if a directory component is a symlink,
    // then any following `..` are resolved relative to the target; see
    // path::normalize() for background).
    //
    // Initially, to fix this, we realized (i.e., realpath(3)) it instead.
    // But that turned out also not to be quite right since now we have all
    // the symlinks resolved: conceptually it feels correct to keep the
    // original header names since that's how the user chose to arrange things
    // and practically this is how the compilers see/report them (e.g., the
    // GCC module mapper).
    //
    // So now we have a pretty elaborate scheme where we try to use the
    // normalized path if possible and fallback to realized. Normalized paths
    // will work for situations where `..` does not cross symlink boundaries,
    // which is the sane case. And for the insane case we only really care
    // about out-of-project files (i.e., system/compiler headers). In other
    // words, if you have the insane case inside your project, then you are on
    // your own.
    //
    void
    normalize_header (path&);
  }
}

#include <libbuild2/cc/utility.ixx>

#endif // LIBBUILD2_CC_UTILITY_HXX
