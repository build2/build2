// file      : build2/test/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/init>

#include <build2/scope>
#include <build2/target>
#include <build2/rule>
#include <build2/diagnostics>

#include <build2/config/utility>

#include <build2/test/module>
#include <build2/test/target>
#include <build2/test/operation>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
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
      auto& vp (var_pool.rw (rs));

      // Tests to execute.
      //
      // Specified as <target>@<path-id> pairs with both sides being optional.
      // The variable is untyped (we want a list of name-pairs), overridable,
      // and inheritable. The target is relative (in essence a prerequisite)
      // which is resolved from the (root) scope where the config.test value
      // is defined.
      //
      vp.insert ("config.test", true);

      // Note: none are overridable.
      //
      // The test variable is a name which can be a path (with the
      // true/false special values) or a target name.
      //
      vp.insert<name>    ("test",           variable_visibility::target);
      vp.insert<name>    ("test.input",     variable_visibility::project);
      vp.insert<name>    ("test.output",    variable_visibility::project);
      vp.insert<name>    ("test.roundtrip", variable_visibility::project);
      vp.insert<strings> ("test.options",   variable_visibility::project);
      vp.insert<strings> ("test.arguments", variable_visibility::project);

      // These are only used in testscript.
      //
      vp.insert<strings> ("test.redirects", variable_visibility::project);
      vp.insert<strings> ("test.cleanups",  variable_visibility::project);

      // Test target platform.
      //
      // Unless already set, default test.target to build.host. Note that it
      // can still be overriden by the user, e.g., in root.build.
      //
      {
        value& v (
          rs.assign (
            vp.insert<target_triplet> (
              "test.target", variable_visibility::project)));

        if (!v || v.empty ())
          v = cast<target_triplet> ((*global_scope)["build.host"]);
      }
    }

    bool
    init (scope& rs,
          scope&,
          const location& l,
          unique_ptr<module_base>& mod,
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

      assert (mod == nullptr);
      mod.reset (new module ());
      module& m (static_cast<module&> (*mod));

      // Configure.
      //
      assert (config_hints.empty ()); // We don't known any hints.

      // Adjust module priority so that the config.test.* values are saved at
      // the end of config.build.
      //
      config::save_module (rs, "test", INT32_MAX);

      // config.test
      //
      if (lookup l = config::omitted (rs, "config.test").first)
      {
        // Figure out which root scope it came from.
        //
        scope* s (&rs);
        for (;
             s != nullptr && !l.belongs (*s);
             s = s->parent_scope ()->root_scope ())
        assert (s != nullptr);

        m.test_ = &cast<names> (l);
        m.root_ = s;
      }

      //@@ TODO: Need ability to specify extra diff options (e.g.,
      //   --strip-trailing-cr, now hardcoded).
      //
      //@@ TODO: Pring report.

      // Register target types.
      //
      {
        auto& t (rs.target_types);

        t.insert<testscript> ();
      }

      // Register rules.
      //
      {
        const rule&        r (m);
        const alias_rule& ar (m);

        // Register our test running rule.
        //
        rs.rules.insert<target> (perform_test_id, "test", r);
        rs.rules.insert<alias> (perform_test_id, "test", ar);

        // Register our rule for the dist meta-operation. We need to do this
        // because we may have ad hoc prerequisites (test input/output files)
        // that need to be entered into the target list.
        //
        rs.rules.insert<target> (dist_id, test_id, "test", r);
      }

      return true;
    }
  }
}
