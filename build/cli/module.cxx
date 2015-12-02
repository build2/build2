// file      : build/cli/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cli/module>

#include <butl/process>
#include <butl/fdstream>

#include <build/scope>
#include <build/target>
#include <build/variable>
#include <build/diagnostics>

#include <build/cxx/target>

#include <build/config/utility>

#include <build/cli/target>
#include <build/cli/rule>

using namespace std;
using namespace butl;

namespace build
{
  namespace cli
  {
    static compile compile_;

    extern "C" bool
    cli_init (scope& root,
              scope& base,
              const location& loc,
              std::unique_ptr<module>&,
              bool first,
              bool optional)
    {
      tracer trace ("cli::init");
      level5 ([&]{trace << "for " << base.out_path ();});

      // Make sure the cxx module has been loaded since we need its
      // targets types (?xx{}). Note that we don't try to load it
      // ourselves because of the non-trivial variable merging
      // semantics. So it is better to let the user load cxx
      // explicitly.
      //
      {
        auto l (base["cxx.loaded"]);

        if (!l || !as<bool> (*l))
          fail (loc) << "cxx module must be loaded before cli";
      }

      // Register target types.
      //
      {
        auto& tts (base.target_types);

        tts.insert<cli> ();
        tts.insert<cli_cxx> ();
      }

      // Enter module variables.
      //
      if (first)
      {
        var_pool.find ("config.cli", string_type); //@@ VAR type

        var_pool.find ("config.cli.options", strings_type);
        var_pool.find ("cli.options", strings_type);
      }

      // Configure.
      //
      // The plan is as follows: try to configure the module. If this
      // fails with the default values and the module is optional,
      // leave it unconfigured.
      //
      bool r (true);

      // We will only honor optional if the user didn't specify any cli
      // configuration explicitly.
      //
      optional = optional && !config::specified (root, "config.cli");

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
            r = false;

            if (verb >= 2)
              text << cli << " not found, leaving cli module unconfigured";
          }
          else
          {
            auto p (config::required (root, "config.cli", cli));
            assert (p.second && as<string> (p.first) == cli);
          }
        }
        else
        {
          auto p (config::required (root, "config.cli", cli));

          // If we actually set a new value, test it by trying to execute.
          //
          if (p.second)
          {
            cli = as<string> (p.first).c_str ();
            ver = test (cli);

            if (ver.empty ())
              throw failed ();
          }
        }

        if (!ver.empty () && verb >= 2)
          text << cli << " " << ver;
      }

      // config.cli.options
      //
      // This one is optional. We also merge it into the corresponding
      // cli.* variables. See the cxx module for more information on
      // this merging semantics and some of its tricky aspects.
      //
      if (r)
      {
        if (const value& v = config::optional (root, "config.cli.options"))
          base.assign ("cli.options") += as<strings> (v);
      }

      // Register our rules.
      //
      if (r)
      {
        auto& rs (base.rules);

        rs.insert<cli_cxx> (perform_id, update_id, "cli", compile_);
        rs.insert<cli_cxx> (perform_id, clean_id, "cli", compile_);

        rs.insert<cxx::hxx> (perform_id, update_id, "cli", compile_);
        rs.insert<cxx::hxx> (perform_id, clean_id, "cli", compile_);

        rs.insert<cxx::cxx> (perform_id, update_id, "cli", compile_);
        rs.insert<cxx::cxx> (perform_id, clean_id, "cli", compile_);

        rs.insert<cxx::ixx> (perform_id, update_id, "cli", compile_);
        rs.insert<cxx::ixx> (perform_id, clean_id, "cli", compile_);

        // Other rules (e.g., cxx::compile) may need to have the group
        // members resolved. Looks like a general pattern: groups should
        // resolve on configure(update).
        //
        rs.insert<cli_cxx> (configure_id, update_id, "cli", compile_);
      }

      return r;
    }
  }
}
