// file      : build2/test/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TEST_MODULE_HXX
#define BUILD2_TEST_MODULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/module.hxx>

#include <build2/test/rule.hxx>
#include <build2/test/common.hxx>

namespace build2
{
  namespace test
  {
    struct module: module_base, virtual common, default_rule, group_rule
    {
      const test::group_rule&
      group_rule () const
      {
        return *this;
      }

      explicit
      module (common_data&& d)
          : common (move (d)),
            test::default_rule (move (d)),
            test::group_rule (move (d)) {}
    };
  }
}

#endif // BUILD2_TEST_MODULE_HXX
