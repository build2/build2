// file      : build2/test/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/operation>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static operation_id
    test_pre (meta_operation_id mo)
    {
      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != disfigure_id ? update_id : 0;
    }

    operation_info test {
      test_id,
      "test",
      "test",
      "testing",
      "has nothing to test", // We cannot "be tested".
      execution_mode::first,
      &test_pre,
      nullptr
    };
  }
}
