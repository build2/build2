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
    config_init (scope& rs,
                 scope& bs,
                 const location&,
                 unique_ptr<module_base>&,
                 bool first,
                 bool optional,
                 const variable_map& hints)
    {
      tracer trace ("cli::config_init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      assert (hints.empty ()); // We don't known any hints.

      // Enter variables.
      //
      if (first)
      {
        auto& v (var_pool);

        // Note: some overridable, some not.
        //
        v.insert<path>    ("config.cli",         true);
        v.insert<strings> ("config.cli.options", true);

        v.insert<process_path> ("cli.path");
        v.insert<strings>      ("cli.options");
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
      bool conf (!optional || !config::unconfigured (rs, "config.cli"));

      if (first)
      {
        // config.cli
        //
        process_path pp;

        // Return version or empty string if the cli executable is not found.
        //
        // @@ This needs some more thinking/cleanup. Specifically, what does
        //    it mean "cli not found"? Is it just not found in PATH? That plus
        //    was not able to execute (e.g., some shared libraries missing)?
        //    That plus cli that we found is something else?
        //
        auto test = [optional, &pp] (const path& cli) -> string
        {
          const char* args[] = {cli.string ().c_str (), "--version", nullptr};

          try
          {
            pp = process::path_search (cli, true); // Can throw.
            args[0] = pp.recall_string ();

            if (verb >= 3)
              print_process (args);

            process pr (pp, args, 0, -1); // Open pipe to stdout.

            try
            {
              ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

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
            catch (const io_error&)
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
              error << "unable to execute " << args[0] << ": " << e.what ();

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
          if (conf)
          {
            ver = test (cli);

            if (ver.empty ())
            {
              // Note that we are unconfigured so that we don't keep
              // re-testing this on each run.
              //
              config::unconfigured (rs, "config.cli", true);
              conf = false;
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

          if (conf)
            dr << "  cli        " << pp << '\n'
               << "  version    " << ver;
          else
            dr << "  cli        " << "not found, leaving unconfigured";
        }

        if (conf)
          rs.assign<process_path> ("cli.path") = move (pp);
      }

      if (conf)
      {
        // config.cli.options
        //
        // This one is optional. We also merge it into the corresponding
        // cli.* variables. See the cxx module for more information on
        // this merging semantics and some of its tricky aspects.
        //
        bs.assign ("cli.options") += cast_null<strings> (
          config::optional (rs, "config.cli.options"));
      }

      return conf;
    }

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          unique_ptr<module_base>&,
          bool,
          bool optional,
          const variable_map& hints)
    {
      tracer trace ("cli::init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // Make sure the cxx module has been loaded since we need its targets
      // types (?xx{}). Note that we don't try to load it ourselves because of
      // the non-trivial variable merging semantics. So it is better to let
      // the user load cxx explicitly.
      //
      if (!cast_false<bool> (bs["cxx.loaded"]))
        fail (loc) << "cxx module must be loaded before cli";

      // Load cli.config.
      //
      if (!cast_false<bool> (bs["cli.config.loaded"]))
      {
        if (!load_module ("cli.config", rs, bs, loc, optional, hints))
          return false;
      }
      else if (!cast_false<bool> (bs["cli.config.configured"]))
      {
        if (!optional)
          fail << "cli module could not be configured" <<
            info << "re-run with -V option for more information";

        return false;
      }

      // Register target types.
      //
      {
        auto& t (bs.target_types);

        t.insert<cli> ();
        t.insert<cli_cxx> ();
      }

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
