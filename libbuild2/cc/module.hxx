// file      : libbuild2/cc/module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_MODULE_HXX
#define LIBBUILD2_CC_MODULE_HXX

#include <unordered_map>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/cc/common.hxx>

#include <libbuild2/cc/compile-rule.hxx>
#include <libbuild2/cc/link-rule.hxx>
#include <libbuild2/cc/install-rule.hxx>
#include <libbuild2/cc/predefs-rule.hxx>

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
      // compiler mode options.
      //
      // This function may also check/set [config.]x.features.* variables on
      // the root scope.
      //
      virtual void
      translate_std (const compiler_info&,
                     const target_triplet&,
                     scope&,
                     strings&,
                     const string*) const = 0;

      const compiler_info* x_info;

      string env_checksum; // Environment checksum (also in x.path).

      // Cached x.internal.scope value.
      //
      using internal_scope = data::internal_scope;
      optional<internal_scope> iscope;
      const scope*             iscope_current = nullptr;

      // Temporary storage for data::sys_*_dirs_*.
      //
      size_t sys_lib_dirs_mode;
      size_t sys_hdr_dirs_mode;
      size_t sys_mod_dirs_mode;

      size_t sys_lib_dirs_extra;
      size_t sys_hdr_dirs_extra;

      bool new_config = false; // See guess() and init() for details.

      // Header cache (see compile_rule::enter_header()).
      //
      // We place it into the config module so that we have an option of
      // sharing it for the entire weak amalgamation.
      //
    public:
      // Keep the hash in the key. This way we can compute it outside of the
      // lock.
      //
      struct header_key
      {
        path   file;
        size_t hash;

        friend bool
        operator== (const header_key& x, const header_key& y)
        {
          return x.file == y.file; // Note: hash was already compared.
        }
      };

      struct header_key_hasher
      {
        size_t operator() (const header_key& k) const {return k.hash;}
      };

      mutable shared_mutex                          header_map_mutex;
      mutable std::unordered_map<header_key,
                                 const file*,
                                 header_key_hasher> header_map;

    private:
      // Defined in gcc.cxx.
      //
      pair<dir_paths, size_t>
      gcc_header_search_dirs (const compiler_info&, scope&) const;

      pair<dir_paths, size_t>
      gcc_library_search_dirs (const compiler_info&, scope&) const;

      // Defined in msvc.cxx.
      //
      pair<dir_paths, size_t>
      msvc_header_search_dirs (const compiler_info&, scope&) const;

      pair<dir_paths, size_t>
      msvc_library_search_dirs (const compiler_info&, scope&) const;
    };

    class LIBBUILD2_CC_SYMEXPORT module: public build2::module,
                                         public virtual common,
                                         public link_rule,
                                         public compile_rule,
                                         public install_rule,
                                         public libux_install_rule,
                                         public predefs_rule
    {
    public:
      explicit
      module (data&& d, const scope& rs)
          : common (move (d)),
            link_rule (move (d)),
            compile_rule (move (d), rs),
            install_rule (move (d), *this),
            libux_install_rule (move (d), *this),
            predefs_rule (move (d)) {}

      void
      init (scope&,
            const location&,
            const variable_map&,
            const compiler_info&);
    };
  }
}

#endif // LIBBUILD2_CC_MODULE_HXX
