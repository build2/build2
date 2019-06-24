// file      : build2/bin/init.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_BIN_INIT_HXX
#define BUILD2_BIN_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

namespace build2
{
  namespace bin
  {
    bool
    vars_init (scope&,
               scope&,
               const location&,
               unique_ptr<module_base>&,
               bool,
               bool,
               const variable_map&);

    bool
    config_init (scope&,
                 scope&,
                 const location&,
                 unique_ptr<module_base>&,
                 bool,
                 bool,
                 const variable_map&);

    bool
    init (scope&,
          scope&,
          const location&,
          unique_ptr<module_base>&,
          bool,
          bool,
          const variable_map&);

    bool
    ar_config_init (scope&,
                    scope&,
                    const location&,
                    unique_ptr<module_base>&,
                    bool,
                    bool,
                    const variable_map&);

    bool
    ar_init (scope&,
             scope&,
             const location&,
             unique_ptr<module_base>&,
             bool,
             bool,
             const variable_map&);

    bool
    ld_config_init (scope&,
                    scope&,
                    const location&,
                    unique_ptr<module_base>&,
                    bool,
                    bool,
                    const variable_map&);

    bool
    ld_init (scope&,
             scope&,
             const location&,
             unique_ptr<module_base>&,
             bool,
             bool,
             const variable_map&);

    bool
    rc_config_init (scope&,
                    scope&,
                    const location&,
                    unique_ptr<module_base>&,
                    bool,
                    bool,
                    const variable_map&);

    bool
    rc_init (scope&,
             scope&,
             const location&,
             unique_ptr<module_base>&,
             bool,
             bool,
             const variable_map&);
  }
}

#endif // BUILD2_BIN_INIT_HXX
