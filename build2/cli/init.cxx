// file      : build2/cli/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cli/init>

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

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          unique_ptr<module_base>&,
          bool first,
          bool optional,
          const variable_map& config_hints)
    {
      tracer trace ("cli::init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      assert (config_hints.empty ()); // We don't known any hints.

      // Make sure the cxx module has been loaded since we need its targets
      // types (?xx{}). Note that we don't try to load it ourselves because of
      // the non-trivial variable merging semantics. So it is better to let
      // the user load cxx explicitly.
      //
      if (!cast_false<bool> (bs["cxx.loaded"]))
        fail (loc) << "cxx module must be loaded before cli";

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
        auto& t (bs.target_types);

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
      optional = optional && !config::specified (rs, "config.cli");

      // If the configuration says we are unconfigured, then we don't need to
      // re-run tests, etc. But we may still need to print the config report.
      //
      bool unconf (optional && config::unconfigured (rs, "config.cli"));

      if (first)
      {
        // config.cli
        //

        // Return version or empty string if unable to execute (e.g., the cli
        // executable is not found).
        //
        auto test = [optional] (const path& cli) -> string
        {
          const char* args[] = {cli.string ().c_str (), "--version", nullptr};

          if (verb >= 3)
            print_process (args);

          try
          {
            process pr (args, 0, -1); // Open pipe to stdout.

            try
            {
              ifdstream is (pr.in_ofd, fdstream_mode::skip);

              // The version should be the last word on the first line.
              //
              string ver;
              getline (is, ver);
              auto p (ver.rfind (' '));
              if (p != string::npos)
                ver = string (ver, p + 1);

              is.close (); // Don't block the other end.

              if (pr.wait ())
              {
                if (ver.empty ())
                  fail << "unexpected output from " << cli;

                return ver;
              }

              // Presumably issued diagnostics. Fall through.
            }
            catch (const ifdstream::failure&)
            {
              pr.wait ();

              // Fall through.
            }

            // Fall through.
          }
          catch (const process_error& e)
          {
            // In some cases this is not enough (e.g., the runtime linker
            // will print scary errors if some shared libraries are not
            // found). So it would be good to redirect child's STDERR.
            //
            if (!optional)
              error << "unable to execute " << cli << ": " << e.what ();

            if (e.child ())
              exit (1);

            // Fall through.
          }

          return string (); // Not found.
        };

        // Adjust module priority (code generator).
        //
        config::save_module (rs, "cli", 150);

        string ver;       // Empty means unconfigured.
        path cli ("cli"); // Default.
        bool nv (false);  // New value.

        if (optional)
        {
          // Test the default value before setting any config.cli.* values
          // so that if we fail to configure, nothing will be written to
          // config.build.
          //
          if (!unconf)
          {
            ver = test (cli);

            if (ver.empty ())
            {
              // Note that we are unconfigured so that we don't keep
              // re-testing this on each run.
              //
              config::unconfigured (rs, "config.cli", true);
              unconf = true;
            }
            else
            {
              auto p (config::required (rs, "config.cli", cli));
              assert (p.second && cast<path> (p.first) == cli);
            }

            nv = true;
          }
        }
        else
        {
          auto p (config::required (rs, "config.cli", cli));

          cli = cast<path> (p.first);
          ver = test (cli);

          if (ver.empty ())
            throw failed (); // Diagnostics already issued.

          nv = p.second;
        }

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (nv ? 2 : 3))
        {
          diag_record dr (text);
          dr << "cli " << project (rs) << '@' << rs.out_path () << '\n';

          if (unconf)
            dr << "  cli        " << "not found, leaving unconfigured";
          else
            dr << "  cli        " << cli << '\n'
               << "  version    " << ver;
        }
      }

      // Nothing else to do if we are unconfigured.
      //
      if (unconf)
        return false;

      // config.cli.options
      //
      // This one is optional. We also merge it into the corresponding
      // cli.* variables. See the cxx module for more information on
      // this merging semantics and some of its tricky aspects.
      //
      bs.assign ("cli.options") += cast_null<strings> (
        config::optional (rs, "config.cli.options"));

      // Register our rules.
      //
      {
        auto& r (bs.rules);

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
