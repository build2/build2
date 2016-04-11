// file      : build2/cli/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cli/module>

#include <build2/scope>
#include <build2/target>
#include <build2/variable>
#include <build2/diagnostics>

#include <build2/cxx/target>

#include <build2/config/utility>

#include <build2/cli/target>
#include <build2/cli/rule>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cli
  {
    static compile compile_;

    extern "C" bool
    cli_init (scope& root,
              scope& base,
              const location& loc,
              unique_ptr<module_base>&,
              bool first,
              bool optional)
    {
      tracer trace ("cli::init");
      l5 ([&]{trace << "for " << base.out_path ();});

      // Make sure the cxx module has been loaded since we need its
      // targets types (?xx{}). Note that we don't try to load it
      // ourselves because of the non-trivial variable merging
      // semantics. So it is better to let the user load cxx
      // explicitly.
      //
      {
        auto l (base["cxx.loaded"]);

        if (!l || !cast<bool> (l))
          fail (loc) << "cxx module must be loaded before cli";
      }

      // Enter module variables.
      //
      if (first)
      {
        auto& v (var_pool);

        // Note: some overridable, some not.
        //
        v.insert<path>    ("config.cli",         true);
        v.insert<strings> ("config.cli.options", true);

        v.insert<strings> ("cli.options");
      }

      // Register target types.
      //
      {
        auto& t (base.target_types);

        t.insert<cli> ();
        t.insert<cli_cxx> ();
      }

      // Configure.
      //
      // The plan is as follows: try to configure the module. If this fails,
      // we are using default values, and the module is optional, leave it
      // unconfigured.
      //

      // We will only honor optional if the user didn't specify any cli
      // configuration explicitly.
      //
      optional = optional && !config::specified (root, "config.cli");

      // Don't re-run tests if the configuration says we are unconfigured.
      //
      if (optional && config::unconfigured (root, "config.cli"))
          return false;

      // config.cli
      //
      if (first)
      {
        // Return version or empty string if unable to execute (e.g.,
        // the cli executable is not found).
        //
        auto test = [optional] (const char* cli) -> string
        {
          const char* args[] = {cli, "--version", nullptr};

          if (verb >= 2)
            print_process (args);
          else if (verb)
            text << "test " << cli;

          string ver;
          try
          {
            process pr (args, 0, -1); // Open pipe to stdout.
            ifdstream is (pr.in_ofd);

            // The version should be the last word on the first line.
            //
            getline (is, ver);
            auto p (ver.rfind (' '));
            if (p != string::npos)
              ver = string (ver, p + 1);

            is.close (); // Don't block the other end.

            if (!pr.wait ())
              return string (); // Not found.

            if (ver.empty ())
              fail << "unexpected output from " << cli;

            return ver;
          }
          catch (const process_error& e)
          {
            // In some cases this is not enough (e.g., the runtime linker
            // will print scary errors if some shared libraries are not
            // found. So it would be good to redirect child's STDERR.
            //
            if (!optional)
              error << "unable to execute " << cli << ": " << e.what ();

            if (e.child ())
              exit (1);

            throw failed ();
          }
        };

        string ver;
        const char* cli ("cli"); // Default.

        if (optional)
        {
          // Test the default value before setting any config.cli.* values
          // so that if we fail to configure, nothing will be written to
          // config.build.
          //
          ver = test (cli);

          if (ver.empty ())
          {
            // Note that we are unconfigured so that we don't keep re-testing
            // this on each run.
            //
            config::unconfigured (root, "config.cli", true);

            if (verb >= 2)
              text << cli << " not found, leaving cli module unconfigured";

            return false;
          }
          else
          {
            auto p (config::required (root, "config.cli", path (cli)));
            assert (p.second && cast<string> (p.first) == cli);
          }
        }
        else
        {
          auto p (config::required (root, "config.cli", path (cli)));

          // If we actually set a new value, test it by trying to execute.
          //
          if (p.second)
          {
            cli = cast<string> (p.first).c_str ();
            ver = test (cli);

            if (ver.empty ())
              throw failed ();
          }
        }

        // Clear the unconfigured flag, if any.
        //
        // @@ Get rid of needing to do this.
        //
        config::unconfigured (root, "config.cli", false);

        if (!ver.empty () && verb >= 2)
          text << cli << " " << ver;
      }

      // config.cli.options
      //
      // This one is optional. We also merge it into the corresponding
      // cli.* variables. See the cxx module for more information on
      // this merging semantics and some of its tricky aspects.
      //
      base.assign ("cli.options") += cast_null<strings> (
        config::optional (root, "config.cli.options"));

      // Register our rules.
      //
      {
        auto& r (base.rules);

        r.insert<cli_cxx> (perform_update_id, "cli.compile", compile_);
        r.insert<cli_cxx> (perform_clean_id, "cli.compile", compile_);

        r.insert<cxx::hxx> (perform_update_id, "cli.compile", compile_);
        r.insert<cxx::hxx> (perform_clean_id, "cli.compile", compile_);

        r.insert<cxx::cxx> (perform_update_id, "cli.compile", compile_);
        r.insert<cxx::cxx> (perform_clean_id, "cli.compile", compile_);

        r.insert<cxx::ixx> (perform_update_id, "cli.compile", compile_);
        r.insert<cxx::ixx> (perform_clean_id, "cli.compile", compile_);

        // Other rules (e.g., cxx::compile) may need to have the group
        // members resolved. Looks like a general pattern: groups should
        // resolve on configure(update).
        //
        r.insert<cli_cxx> (configure_update_id, "cli.compile", compile_);
      }

      return true;
    }
  }
}
