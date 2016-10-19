// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/runner>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      static void
      print_test (diag_record& r, const test& t)
      {
        // @@ No indentation performed for here-documents. If to fix then
        // probably need to do on diag_record level in a way similar to
        // butl::pager approach.
        //
        r << t;
      }

      static void
      print_test (const test& t)
      {
        diag_record r (text);
        print_test (r, t);
      }

      void concurrent_runner::
      run (const test& t)
      {
        // @@ TODO

        // @@ When running multiple threads will need to synchronize printing
        // the diagnostics so it don't overlap for concurrent tests.
        // Alternatively we can not bother with that and expect a user to
        // re-run test operation in the single-thread mode.
        //

        if (verb >= 3)
          print_test (t);
      }
    }
  }
}
