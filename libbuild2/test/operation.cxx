// file      : libbuild2/test/operation.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/operation.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>

#include <libbuild2/test/common.hxx> // test_deadline()

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static operation_id
    pre_test (context&, const values&, meta_operation_id mo, const location&)
    {
      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != disfigure_id ? update_id : 0;
    }

    // Ad hoc rule apply callback.
    //
    // If this is not perform(test) or there is no deadline set for the test
    // execution, then forward the call to the ad hoc rule's apply().
    // Otherwise, return a recipe that will execute with the deadline if we
    // can get it and return the noop recipe that just issues a warning if we
    // can't.
    //
    static recipe
    adhoc_apply (const adhoc_rule& ar, action a, target& t, match_extra& me)
    {
      optional<timestamp> d;

      if (a != perform_test_id || !(d = test_deadline (t)))
        return ar.apply (a, t, me);

      if (const auto* dr = dynamic_cast<const adhoc_rule_with_deadline*> (&ar))
      {
        if (recipe r = dr->apply (a, t, me, d))
          return r;
      }

      return [] (action a, const target& t)
      {
        warn << "unable to impose timeout on test for target " << t
             << ", skipping";
        return noop_action (a, t);
      };
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
      &pre_test,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &adhoc_apply
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
      op_update.pre_operation,
      op_update.post_operation,
      op_update.operation_pre,
      op_update.operation_post,
      op_update.adhoc_match,
      op_update.adhoc_apply
    };
  }
}
