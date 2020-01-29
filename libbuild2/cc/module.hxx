// file      : libbuild2/cc/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_MODULE_HXX
#define LIBBUILD2_CC_MODULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/cc/common.hxx>

#include <libbuild2/cc/compile-rule.hxx>
#include <libbuild2/cc/link-rule.hxx>
#include <libbuild2/cc/install-rule.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  namespace cc
  {
    struct compiler_info;

    class LIBBUILD2_CC_SYMEXPORT config_module: public build2::module,
                                                public virtual config_data
    {
    public:
      explicit
      config_module (config_data&& d) : config_data (move (d)) {}

      // We split the configuration process into into two parts: guessing the
      // compiler information and the actual configuration. This allows one to
      // adjust configuration (say the standard or enabled experimental
      // features) base on the compiler information by first loading the
      // guess module.
      //
      void
      guess (scope&, const location&, const variable_map&);

      void
      init (scope&, const location&, const variable_map&);

      // Translate the x.std value (if any) to the standard-selecting
      // option(s) (if any). May also check/set x.features.* variables on the
      // root scope.
      //
      virtual strings
      translate_std (const compiler_info&, scope&, const string*) const = 0;

      strings tstd;

      const compiler_info* x_info;

      // Temporary storage for data::sys_*_dirs_extra.
      //
      size_t sys_lib_dirs_extra;
      size_t sys_inc_dirs_extra;

    private:
      // Defined in gcc.cxx.
      //
      dir_paths
      gcc_header_search_paths (const process_path&, scope&) const;

      dir_paths
      gcc_library_search_paths (const process_path&, scope&) const;

      // Defined in msvc.cxx.
      //
      dir_paths
      msvc_header_search_paths (const process_path&, scope&) const;

      dir_paths
      msvc_library_search_paths (const process_path&, scope&) const;

    private:
      bool new_; // See guess() and init() for details.
    };

    class LIBBUILD2_CC_SYMEXPORT module: public build2::module,
                                         public virtual common,
                                         link_rule,
                                         compile_rule,
                                         install_rule,
                                         libux_install_rule
    {
    public:
      explicit
      module (data&& d)
          : common (move (d)),
            link_rule (move (d)),
            compile_rule (move (d)),
            install_rule (move (d), *this),
            libux_install_rule (move (d), *this) {}

      void
      init (scope&, const location&, const variable_map&);
    };
  }
}

#endif // LIBBUILD2_CC_MODULE_HXX
