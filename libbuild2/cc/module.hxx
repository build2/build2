// file      : libbuild2/cc/module.hxx -*- C++ -*-
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
                                                public config_data
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
      // option(s) (if any) and fold them (normally by pre-pending) into the
      // compiler mode options. This function may also check/set x.features.*
      // variables on the root scope.
      //
      virtual void
      translate_std (const compiler_info&,
                     const target_triplet&,
                     scope&,
                     strings&,
                     const string*) const = 0;

      const compiler_info* x_info;

      // Temporary storage for data::sys_*_dirs_*.
      //
      size_t sys_lib_dirs_mode;
      size_t sys_inc_dirs_mode;
      size_t sys_mod_dirs_mode;

      size_t sys_lib_dirs_extra;
      size_t sys_inc_dirs_extra;

      bool new_config = false; // See guess() and init() for details.

    private:
      // Defined in gcc.cxx.
      //
      pair<dir_paths, size_t>
      gcc_header_search_dirs (const process_path&, scope&) const;

      pair<dir_paths, size_t>
      gcc_library_search_dirs (const process_path&, scope&) const;

      // Defined in msvc.cxx.
      //
      pair<dir_paths, size_t>
      msvc_header_search_dirs (const process_path&, scope&) const;

      pair<dir_paths, size_t>
      msvc_library_search_dirs (const process_path&, scope&) const;
    };

    class LIBBUILD2_CC_SYMEXPORT module: public build2::module,
                                         public virtual common,
                                         public link_rule,
                                         public compile_rule,
                                         public install_rule,
                                         public libux_install_rule
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
      init (scope&,
            const location&,
            const variable_map&,
            const compiler_info&);
    };
  }
}

#endif // LIBBUILD2_CC_MODULE_HXX
