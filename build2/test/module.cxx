// file      : build2/test/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/module>

#include <build2/scope>
#include <build2/target>
#include <build2/rule>
#include <build2/diagnostics>

#include <build2/test/operation>
#include <build2/test/rule>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static rule rule_;

    extern "C" void
    test_boot (scope& root, const location&, unique_ptr<module>&)
    {
      tracer trace ("test::boot");

      l5 ([&]{trace << "for " << root.out_path ();});

      // Register the test operation.
      //
      root.operations.insert (test_id, test);

      // Enter module variables. Do it during boot in case they get assigned
      // in bootstrap.build.
      //
      {
        auto& v (var_pool);

        v.find ("test", bool_type);
        v.find ("test.input", name_type);
        v.find ("test.output", name_type);
        v.find ("test.roundtrip", name_type);
        v.find ("test.options", strings_type);
        v.find ("test.arguments", strings_type);
      }
    }

    extern "C" bool
    test_init (scope& root,
               scope&,
               const location& l,
               unique_ptr<module>&,
               bool first,
               bool)
    {
      tracer trace ("test::init");

      if (!first)
      {
        warn (l) << "multiple test module initializations";
        return true;
      }

      const dir_path& out_root (root.out_path ());
      l5 ([&]{trace << "for " << out_root;});

      // Register rules.
      //
      {
        auto& r (root.rules);

        // Register our test running rule.
        //
        r.insert<target> (perform_test_id, "test", rule_);

        // Register our rule for the dist meta-operation. We need
        // to do this because we have "ad-hoc prerequisites" (test
        // input/output files) that need to be entered into the
        // target list.
        //
        r.insert<target> (dist_id, test_id, "test", rule_);
      }

      return true;
    }
  }
}
