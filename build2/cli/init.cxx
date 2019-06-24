// file      : build2/cli/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cli/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <build2/cxx/target.hxx>

#include <build2/config/utility.hxx>

#include <build2/cli/target.hxx>
#include <build2/cli/rule.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cli
  {
    static const compile_rule compile_rule_;

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& l,
                 unique_ptr<module_base>&,
                 bool first,
                 bool optional,
                 const variable_map& hints)
    {
      tracer trace ("cli::config_init");
      l5 ([&]{trace << "for " << bs;});

      assert (hints.empty ()); // We don't known any hints.

      // Enter variables.
      //
      if (first)
      {
        auto& v (var_pool.rw (rs));

        // Note: some overridable, some not.
        //
        // The config.cli=false is recognized as an explicit request to leave
        // the module unconfigured.
        //
        v.insert<path>    ("config.cli",         true);
        v.insert<strings> ("config.cli.options", true);

        //@@ TODO: split version into componets (it is stdver).
        //
        v.insert<process_path> ("cli.path");
        v.insert<string>       ("cli.version");
        v.insert<string>       ("cli.checksum");
        v.insert<strings>      ("cli.options");
      }

      // Configure.
      //
      // The plan is as follows: try to configure the module. If this fails,
      // we are using default values, and the module is optional, leave it
      // unconfigured.

      // First take care of the explicit request by the user to leave the
      // module unconfigured.
      //
      bool conf (true);

      if (const path* p = cast_null<path> (rs["config.cli"]))
      {
        conf = p->string () != "false";

        if (!conf && !optional)
          fail (l) << "non-optional module requested to be left unconfigured";
      }

      if (conf)
      {
        // Otherwise we will only honor optional if the user didn't specify
        // any cli configuration explicitly.
        //
        optional = optional && !config::specified (rs, "cli");

        // If the configuration says we are unconfigured, then we should't
        // re-run tests, etc. But we may still need to print the config
        // report.
        //
        conf = !optional || !config::unconfigured (rs, "cli");
      }

      if (first)
      {
        // config.cli
        //
        process_path pp;

        // Return version or empty string if the cli executable is not found
        // or is not the command line interface compiler.
        //
        // @@ This needs some more thinking/cleanup. Specifically, what does
        //    it mean "cli not found"? Is it just not found in PATH? That plus
        //    was not able to execute (e.g., some shared libraries missing)?
        //    That plus cli that we found is something else?
        //
        auto test = [optional, &pp] (const path& cli) -> string
        {
          const char* args[] = {cli.string ().c_str (), "--version", nullptr};

          // @@ TODO: redo using run_start()/run_finish() or even
          //    run<string>(). We have the ability to ignore exit code and
          //    redirect STDERR to STDOUT.

          try
          {
            // Only search in PATH (specifically, omitting the current
            // executable's directory on Windows).
            //
            pp = process::path_search (cli,
                                       true        /* init */,
                                       dir_path () /* fallback */,
                                       true        /* path_only */);
            args[0] = pp.recall_string ();

            if (verb >= 3)
              print_process (args);

            process pr (pp, args, 0, -1); // Open pipe to stdout.

            try
            {
              ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

              // The version should be the last word on the first line. But
              // also check the prefix since there are other things called
              // 'cli', for example, "Mono JIT compiler".
              //
              string v;
              getline (is, v);

              if (v.compare (0, 37,
                             "CLI (command line interface compiler)") == 0)
              {
                size_t p (v.rfind (' '));

                if (p == string::npos)
                  fail << "unexpected output from " << cli;

                v.erase (0, p + 1);
              }
              else
              {
                if (!optional)
                  fail << cli << " is not command line interface compiler" <<
                    info << "use config.cli to override";

                v.clear ();
              }

              is.close (); // Don't block the other end.

              if (pr.wait ())
                return v;

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
              error << "unable to execute " << args[0] << ": " << e <<
                info << "use config.cli to override";

            if (e.child)
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
              conf = false;
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

        string checksum;
        if (conf)
        {
          // Hash the compiler path and version.
          //
          sha256 cs;
          cs.append (pp.effect_string ());
          cs.append (ver);
          checksum = cs.string ();
        }
        else
        {
          // Note that we are unconfigured so that we don't keep re-testing
          // this on each run.
          //
          nv = config::unconfigured (rs, "cli", true) || nv;
        }

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (nv ? 2 : 3))
        {
          diag_record dr (text);
          dr << "cli " << project (rs) << '@' << rs << '\n';

          if (conf)
            dr << "  cli        " << pp << '\n'
               << "  version    " << ver << '\n'
               << "  checksum   " << checksum;
          else
            dr << "  cli        " << "not found, leaving unconfigured";
        }

        if (conf)
        {
          rs.assign ("cli.path")     = move (pp);
          rs.assign ("cli.version")  = move (ver);
          rs.assign ("cli.checksum") = move (checksum);
        }
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
          const location& l,
          unique_ptr<module_base>&,
          bool,
          bool optional,
          const variable_map& hints)
    {
      tracer trace ("cli::init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure the cxx module has been loaded since we need its targets
      // types (?xx{}). Note that we don't try to load it ourselves because of
      // the non-trivial variable merging semantics. So it is better to let
      // the user load cxx explicitly.
      //
      if (!cast_false<bool> (bs["cxx.loaded"]))
        fail (l) << "cxx module must be loaded before cli";

      // Load cli.config.
      //
      if (!cast_false<bool> (bs["cli.config.loaded"]))
      {
        if (!load_module (rs, bs, "cli.config", l, optional, hints))
          return false;
      }
      else if (!cast_false<bool> (bs["cli.config.configured"]))
      {
        if (!optional)
          fail (l) << "cli module could not be configured" <<
            info << "re-run with -V for more information";

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

        auto reg = [&r] (meta_operation_id mid, operation_id oid)
        {
          r.insert<cli_cxx>  (mid, oid, "cli.compile", compile_rule_);
          r.insert<cxx::hxx> (mid, oid, "cli.compile", compile_rule_);
          r.insert<cxx::cxx> (mid, oid, "cli.compile", compile_rule_);
          r.insert<cxx::ixx> (mid, oid, "cli.compile", compile_rule_);
        };

        reg (perform_id, update_id);
        reg (perform_id, clean_id);

        // Other rules (e.g., cxx::compile) may need to have the group members
        // resolved/linked up. Looks like a general pattern: groups should
        // resolve on *(update).
        //
        // @@ meta-op wildcard?
        //
        reg (configure_id, update_id);
        reg (dist_id, update_id);
      }

      return true;
    }
  }
}
