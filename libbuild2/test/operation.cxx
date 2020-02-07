// file      : libbuild2/test/operation.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/operation.hxx>

#include <libbuild2/variable.hxx>

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
      0,
      "test",
      "test",
      "testing",
      "tested",
      "has nothing to test", // We cannot "be tested".
      execution_mode::first,
      1 /* concurrency */,
      &test_pre,
      nullptr
    };

    // Also the explicit update-for-test operation alias.
    //
    const operation_info op_update_for_test {
      update_id, // Note: not update_for_test_id.
      test_id,
      op_update.name,
      op_update.name_do,
      op_update.name_doing,
      op_update.name_did,
      op_update.name_done,
      op_update.mode,
      op_update.concurrency,
      op_update.pre,
      op_update.post
    };
  }
}
