// file      : libbuild2/dynamic.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/dynamic.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  bool
  update_during_match (tracer& trace, action a, const target& t, timestamp ts)
  {
    // In particular, this function is used to make sure header dependencies
    // are up to date.
    //
    // There would normally be a lot of headers for every source file (think
    // all the system headers) and just calling execute_direct() on all of
    // them can get expensive. At the same time, most of these headers are
    // existing files that we will never be updating (again, system headers,
    // for example) and the rule that will match them is the fallback
    // file_rule. That rule has an optimization: it returns noop_recipe (which
    // causes the target state to be automatically set to unchanged) if the
    // file is known to be up to date. So we do the update "smartly".
    //
    const path_target* pt (t.is_a<path_target> ());

    if (pt == nullptr)
      ts = timestamp_unknown;

    target_state os (t.matched_state (a));

    if (os == target_state::unchanged)
    {
      if (ts == timestamp_unknown)
        return false;
      else
      {
        // We expect the timestamp to be known (i.e., existing file).
        //
        timestamp mt (pt->mtime ());
        assert (mt != timestamp_unknown);
        return mt > ts;
      }
    }
    else
    {
      // We only want to return true if our call to execute() actually caused
      // an update. In particular, the target could already have been in
      // target_state::changed because of the dynamic dependency extraction
      // run for some other target.
      //
      // @@ MT perf: so we are going to switch the phase and execute for
      //    any generated header.
      //
      phase_switch ps (t.ctx, run_phase::execute);
      target_state ns (execute_direct (a, t));

      if (ns != os && ns != target_state::unchanged)
      {
        l6 ([&]{trace << "updated " << t
                      << "; old state " << os
                      << "; new state " << ns;});
        return true;
      }
      else
        return ts != timestamp_unknown ? pt->newer (ts, ns) : false;
    }
  }
}
