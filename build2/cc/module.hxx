// file      : build2/cc/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_MODULE_HXX
#define BUILD2_CC_MODULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/module.hxx>
#include <build2/variable.hxx>

#include <build2/cc/common.hxx>

#include <build2/cc/compile.hxx>
#include <build2/cc/link.hxx>
#include <build2/cc/install.hxx>

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

      void
      init (scope&, const location&, const variable_map&);

      // Translate the x.std value (if any) to the standard-selecting
      // option(s) (if any). May also check/set x.features.* variables on the
      // root scope.
      //
      virtual strings
      translate_std (const compiler_info&, scope&, const string*) const = 0;

      strings tstd;

      compiler_id::value_type cid;

    private:
      dir_paths
      gcc_library_search_paths (process_path&, scope&) const; // gcc.cxx

      dir_paths
      msvc_library_search_paths (process_path&, scope&) const; // msvc.cxx
    };

    class module: public module_base, protected virtual common,
                  link,
                  compile,
                  file_install,
                  alias_install
    {
    public:
      explicit
      module (data&& d)
          : common (move (d)),
            link (move (d)),
            compile (move (d)),
            file_install (move (d), *this),
            alias_install (move (d), *this) {}

      void
      init (scope&, const location&, const variable_map&);
    };
  }
}

#endif // BUILD2_CC_MODULE_HXX
