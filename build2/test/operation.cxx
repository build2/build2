// file      : build2/test/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/operation.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static operation_id
    test_pre (const values& params, meta_operation_id mo, const location& l)
    {
      if (!params.empty ())
        fail (l) << "unexpected parameters for operation test";

      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != disfigure_id ? update_id : 0;
    }

    const operation_info op_test {
      test_id,
      "test",
      "test",
      "testing",
      "tested",
      "has nothing to test", // We cannot "be tested".
      execution_mode::first,
      1,
      &test_pre,
      nullptr
    };
  }
}
