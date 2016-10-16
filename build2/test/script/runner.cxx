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
      void concurrent_runner::
      run (const test& t)
      {
        // @@ TODO
        text << "run " << t.program.string ();
      }
    }
  }
}
