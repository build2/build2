// file      : libbuild2/test/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/rule.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/script/timeout.hxx>

#include <libbuild2/test/module.hxx>
#include <libbuild2/test/target.hxx>
#include <libbuild2/test/operation.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static const file_rule file_rule_ (true /* check_type */);

    void
    boot (scope& rs, const location&, module_boot_extra& extra)
    {
      tracer trace ("test::boot");

      l5 ([&]{trace << "for " << rs;});

      // Enter module variables. Do it during boot in case they get assigned
      // in bootstrap.build.
      //
      // Most of the variables we enter are qualified so go straight for the
      // public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));
      auto& pvp (rs.var_pool ()); // For `test` and `for_test`.

      common_data d {

        // Tests to execute.
        //
        // Specified as <target>@<path-id> pairs with both sides being
        // optional. The variable is untyped (we want a list of name-pairs),
        // overridable, and with global visibility. The target is relative (in
        // essence a prerequisite) which is resolved from the (root) scope
        // where the config.test value is defined.
        //
        vp.insert ("config.test"),

        // Test working directory before/after cleanup (see Testscript spec
        // for semantics).
        //
        vp.insert<name_pair> ("config.test.output"),

        // Test operation and individual test execution timeouts (see the
        // manual for semantics).
        //
        vp.insert<string> ("config.test.timeout"),

        // Test command runner path and options (see the manual for semantics).
        //
        vp.insert<strings> ("config.test.runner"),

        // The test variable is a name which can be a path (with the
        // true/false special values) or a target name.
        //
        pvp.insert<name>   ("test", variable_visibility::target),
        vp.insert<strings> ("test.options"),
        vp.insert<strings> ("test.arguments"),

        // Test command runner path and options extracted from
        // config.test.runner.
        //
        vp.insert<process_path> ("test.runner.path"),
        vp.insert<strings>      ("test.runner.options"),

        // Prerequisite-specific.
        //
        // test.stdin and test.stdout can be used to mark a prerequisite as a
        // file to redirect stdin from and to compare stdout to, respectively.
        // test.roundtrip is a shortcut to mark a prerequisite as both stdin
        // and stdout.
        //
        // Prerequisites marked with test.input are treated as additional test
        // inputs: they are made sure to be up to date and their paths are
        // passed as additional command line arguments (after test.options and
        // test.arguments). Their primary use is to pass inputs that may have
        // varying file names/paths, for example:
        //
        // exe{parent}: exe{child}: test.input = true
        //
        // Note that currently this mechanism is only available to simple
        // tests though we could also support it for testscript (e.g., by
        // appending the input paths to test.arguments or by passing them in a
        // separate test.inputs variable).
        //
        vp.insert<bool> ("test.stdin",     variable_visibility::prereq),
        vp.insert<bool> ("test.stdout",    variable_visibility::prereq),
        vp.insert<bool> ("test.roundtrip", variable_visibility::prereq),
        vp.insert<bool> ("test.input",     variable_visibility::prereq),

        // Test target platform.
        //
        vp.insert<target_triplet> ("test.target")
      };

      // This one is used by other modules/rules.
      //
      pvp.insert<bool> ("for_test", variable_visibility::prereq);

      // These are only used in testscript.
      //
      vp.insert<cmdline> ("test.redirects");
      vp.insert<cmdline> ("test.cleanups");

      // Unless already set, default test.target to build.host. Note that it
      // can still be overriden by the user, e.g., in root.build.
      //
      {
        value& v (rs.assign (d.test_target));

        if (!v || v.empty ())
          v = *rs.ctx.build_host;
      }

      // Register our operations.
      //
      rs.insert_operation (test_id,            op_test,            &d.var_test);
      rs.insert_operation (update_for_test_id, op_update_for_test, &d.var_test);

      extra.set_module (new module (move (d)));
    }

    bool
    init (scope& rs,
          scope&,
          const location& l,
          bool first,
          bool,
          module_init_extra& extra)
    {
      tracer trace ("test::init");

      if (!first)
      {
        warn (l) << "multiple test module initializations";
        return true;
      }

      l5 ([&]{trace << "for " << rs;});

      auto& m (extra.module_as<module> ());

      // Configuration.
      //
      using config::lookup_config;

      // Adjust module priority so that the config.test.* values are saved at
      // the end of config.build.
      //
      config::save_module (rs, "test", INT32_MAX);

      // config.test
      //
      if (lookup l = lookup_config (rs, m.config_test))
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

      // config.test.output
      //
      if (lookup l = lookup_config (rs, m.config_test_output))
      {
        const name_pair& p (cast<name_pair> (l));

        // If second half is empty, then first is the after value.
        //
        const name& a (p.second.empty () ? p.first  : p.second); // after
        const name& b (p.second.empty () ? p.second : p.first);  // before

        // Parse and validate.
        //
        if (!b.simple ())
          fail << "invalid config.test.output before value '" << b << "'";

        if (!a.simple ())
          fail << "invalid config.test.output after value '" << a << "'";

        if      (a.value == "clean") m.after = output_after::clean;
        else if (a.value == "keep")  m.after = output_after::keep;
        else fail << "invalid config.test.output after value '" << a << "'";

        if      (b.value == "fail")  m.before = output_before::fail;
        else if (b.value == "warn")  m.before = output_before::warn;
        else if (b.value == "clean") m.before = output_before::clean;
        else if (b.value == "")      m.before = output_before::clean;
        else fail << "invalid config.test.output before value '" << b << "'";
      }

      // config.test.timeout
      //
      if (lookup l = lookup_config (rs, m.config_test_timeout))
      {
        const string& t (cast<string> (l));

        const char* ot ("config.test.timeout test operation timeout value");
        const char* tt ("config.test.timeout test timeout value");

        size_t p (t.find ('/'));
        if (p != string::npos)
        {
          // Note: either of the timeouts can be omitted but not both.
          //
          if (t.size () == 1)
            fail << "invalid config.test.timeout value '" << t << "'";

          if (p != 0)
            m.operation_timeout = parse_timeout (string (t, 0, p), ot);

          if (p != t.size () - 1)
            m.test_timeout = parse_timeout (string (t, p + 1), tt);
        }
        else
          m.operation_timeout = parse_timeout (t, ot);
      }

      // config.test.runner
      //
      {
        value& pv (rs.assign (m.test_runner_path));
        value& ov (rs.assign (m.test_runner_options));

        if (lookup l = lookup_config (rs, m.config_test_runner))
        {
          const strings& args (cast<strings> (l));

          // Extract the runner process path.
          //
          {
            const string& s (args.empty () ? empty_string : args.front ());

            path p;
            try { p = path (s); } catch (const invalid_path&) {}

            if (p.empty ())
              fail << "invalid runner path '" << s << "' in "
                   << m.config_test_runner;

            pv = run_search (p, false /* init */);
            m.runner_path = &pv.as<process_path> ();
          }

          // Extract the runner options.
          //
          {
            ov = strings (++args.begin (), args.end ());
            m.runner_options = &ov.as<strings> ();
          }
        }
        else
        {
          pv = nullptr;
          ov = nullptr;
        }
      }

      //@@ TODO: Need ability to specify extra diff options (e.g.,
      //   --strip-trailing-cr, now hardcoded).
      //
      //@@ TODO: Pring report.

      // Environment.
      //
      // The only external program that we currently use is diff. None of the
      // implementations we looked at (GNU diffutils, FreeBSD) use any
      // environment variables.

      // Register target types.
      //
      {
        auto& tt (rs.insert_target_type<testscript> ());
        rs.insert_target_type_file ("testscript", tt);
      }

      // Register our test running rule.
      //
      {
        default_rule& dr (m);

        rs.insert_rule<target> (perform_test_id, "test", dr);
        rs.insert_rule<alias>  (perform_test_id, "test", dr);

        // Register the fallback file rule for the update-for-test operation,
        // similar to update.
        //
        // Note: use target instead of anything more specific (such as
        // mtime_target) in order not to take precedence over the "test" rule
        // above.
        //
        rs.global_scope ().insert_rule<target> (
          perform_test_id, "test.file", file_rule_);
      }

      return true;
    }

    static const module_functions mod_functions[] =
    {
      {"test",  &boot,   &init},
      {nullptr, nullptr, nullptr}
    };

    const module_functions*
    build2_test_load ()
    {
      return mod_functions;
    }
  }
}
