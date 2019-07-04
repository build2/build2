// file      : libbuild2/test/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_MODULE_HXX
#define LIBBUILD2_TEST_MODULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/test/rule.hxx>
#include <libbuild2/test/common.hxx>

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

#endif // LIBBUILD2_TEST_MODULE_HXX
