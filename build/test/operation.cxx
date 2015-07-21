// file      : build/test/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/test/operation>

#include <build/config/operation>

using namespace std;
using namespace butl;

namespace build
{
  namespace test
  {
    static operation_id
    test_pre (meta_operation_id mo)
    {
      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != config::disfigure_id ? update_id : 0;
    }

    operation_info test {
      "test",
      "test",
      "testing",
      "tested",
      execution_mode::first,
      &test_pre,
      nullptr
    };
  }
}
