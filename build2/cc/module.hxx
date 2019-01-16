// file      : build2/cc/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_MODULE_HXX
#define BUILD2_CC_MODULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/module.hxx>
#include <build2/variable.hxx>

#include <build2/cc/common.hxx>

#include <build2/cc/compile-rule.hxx>
#include <build2/cc/link-rule.hxx>
#include <build2/cc/install-rule.hxx>

namespace build2
{
  namespace cc
  {
    struct compiler_info;

    class config_module: public module_base, public virtual config_data
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
      size_t sys_lib_dirs_extra; // First extra path (size if none).
      size_t sys_inc_dirs_extra; // First extra path (size if none).

      const compiler_info* ci_;

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

    class module: public module_base, public virtual common,
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

#endif // BUILD2_CC_MODULE_HXX
