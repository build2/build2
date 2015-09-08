// file      : build/test/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/test/module>

#include <build/scope>
#include <build/target>
#include <build/rule>
#include <build/diagnostics>

#include <build/test/operation>
#include <build/test/rule>

using namespace std;
using namespace butl;

namespace build
{
  namespace test
  {
    static rule rule_;

    extern "C" void
    test_init (scope& r,
               scope& b,
               const location& l,
               unique_ptr<build::module>&,
               bool first)
    {
      tracer trace ("test::init");

      if (&r != &b)
        fail (l) << "test module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple test module initializations";
        return;
      }

      const dir_path& out_root (r.out_path ());
      level5 ([&]{trace << "for " << out_root;});

      // Register the test operation.
      //
      r.operations.insert (test_id, test);

      // Register rules.
      //
      {
        auto& rs (r.rules);

        // Register our test running rule.
        //
        rs.insert<target> (perform_id, test_id, "test", rule_);

        // Register our rule for the dist meta-operation. We need
        // to do this because we have "ad-hoc prerequisites", test
        // input/output files, that need to be entered into the
        // target list.
        //
        rs.insert<target> (dist_id, test_id, "test", rule_);
      }

      // Enter module variables.
      //
      {
        variable_pool.find ("test", bool_type);
        variable_pool.find ("test.input", name_type);
        variable_pool.find ("test.output", name_type);
        variable_pool.find ("test.roundtrip", name_type);
        variable_pool.find ("test.options", strings_type);
        variable_pool.find ("test.arguments", strings_type);
      }
    }
  }
}
