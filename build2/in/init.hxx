// file      : build2/in/init.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_IN_INIT_HXX
#define BUILD2_IN_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

namespace build2
{
  namespace in
  {
    bool
    base_init (scope&,
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

#endif // BUILD2_IN_INIT_HXX
