// file      : build2/test/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/init>

#include <build2/scope>
#include <build2/target>
#include <build2/rule>
#include <build2/diagnostics>

#include <build2/test/rule>
#include <build2/test/target>
#include <build2/test/operation>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static rule rule_;

    void
    boot (scope& rs, const location&, unique_ptr<module_base>&)
    {
      tracer trace ("test::boot");

      l5 ([&]{trace << "for " << rs.out_path ();});

      // Register the test operation.
      //
      rs.operations.insert (test_id, test);

      // Enter module variables. Do it during boot in case they get assigned
      // in bootstrap.build.
      //
      {
        auto& v (var_pool);

        // Note: none are overridable.
        //
        // The test variable is a name which can be a path (with the
        // true/false special values) or a target name.
        //
        v.insert<name>    ("test",           variable_visibility::target);
        v.insert<name>    ("test.input",     variable_visibility::project);
        v.insert<name>    ("test.output",    variable_visibility::project);
        v.insert<name>    ("test.roundtrip", variable_visibility::project);
        v.insert<strings> ("test.options",   variable_visibility::project);
        v.insert<strings> ("test.arguments", variable_visibility::project);

        // These are only used in testscript.
        //
        v.insert<strings> ("test.redirects", variable_visibility::project);
        v.insert<strings> ("test.cleanups", variable_visibility::project);
      }
    }

    bool
    init (scope& rs,
          scope&,
          const location& l,
          unique_ptr<module_base>&,
          bool first,
          bool,
          const variable_map& config_hints)
    {
      tracer trace ("test::init");

      if (!first)
      {
        warn (l) << "multiple test module initializations";
        return true;
      }

      const dir_path& out_root (rs.out_path ());
      l5 ([&]{trace << "for " << out_root;});

      assert (config_hints.empty ()); // We don't known any hints.

      //@@ TODO: Need ability to specify extra diff options (e.g.,
      //   --strip-trailing-cr, now hardcoded).

      // Adjust module priority so that the config.test.* values are saved at
      // the end of config.build.
      //
      // if (s)
      //   config::save_module (r, "test", INT32_MAX);

      // Register target types.
      //
      {
        auto& t (rs.target_types);

        t.insert<testscript> ();
      }

      // Register rules.
      //
      {
        auto& r (rs.rules);

        // Register our test running rule.
        //
        r.insert<target> (perform_test_id, "test", rule_);
        r.insert<alias> (perform_test_id, "test", rule_); // Override generic.

        // Register our rule for the dist meta-operation. We need to do this
        // because we may have ad hoc prerequisites (test input/output files)
        // that need to be entered into the target list.
        //
        r.insert<target> (dist_id, test_id, "test", rule_);
      }

      return true;
    }
  }
}
