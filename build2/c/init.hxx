// file      : build2/c/init.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_C_INIT_HXX
#define BUILD2_C_INIT_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/module.hxx>

namespace build2
{
  namespace c
  {
    bool
    guess_init (scope&,
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
  }
}

#endif // BUILD2_C_INIT_HXX
