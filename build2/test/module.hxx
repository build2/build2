// file      : build2/test/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
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
    struct module: module_base, virtual common, rule, alias_rule
    {
    };
  }
}

#endif // BUILD2_TEST_MODULE_HXX
