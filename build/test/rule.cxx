// file      : build/test/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/test/rule>

#include <build/scope>
#include <build/target>
#include <build/algorithm>
#include <build/diagnostics>

using namespace std;

namespace build
{
  namespace test
  {
    match_result rule::
    match (action a, target& t, const std::string&) const
    {
      // First determine if this is a test.
      //
      auto v (t.vars["test"]);

      if (!v)
        v.rebind (t.base_scope ()[string("test.") + t.type ().name]);

      if (!v || !v.as<bool> ())
        return match_result (t, false); // "Not a test" result.

      // If this is the update pre-operation, make someone else do
      // the job.
      //
      if (a.operation () != test_id)
        return nullptr;

      return match_result (t, true);
    }

    recipe rule::
    apply (action, target&, const match_result& mr) const
    {
      if (!mr.value) // Not a test.
        return noop_recipe;

      return noop_recipe; //@@ TMP
    }
  }
}
