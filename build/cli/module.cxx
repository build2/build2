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

    extern "C" void
    cli_init (scope& root,
              scope& base,
              const location& l,
              std::unique_ptr<module>&,
              bool first)
    {
      tracer trace ("cli::init");
      level5 ([&]{trace << "for " << base.out_path ();});

      // Make sure the cxx module has been loaded since we need its
      // targets types (?xx{}). Note that we don't try to load it
      // ourselves because of the non-trivial variable merging
      // semantics. So it is better to let the user load cxx
      // explicitly.
      //
      if (base.find_target_type ("cxx") == nullptr)
        fail (l) << "cxx module must be initialized before cli";

      // Register target types.
      //
      {
        auto& tts (base.target_types);

        tts.insert<cli> ();
        tts.insert<cli_cxx> ();
      }

      // Register our rules.
      //
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
      }

      // Enter module variables.
      //
      if (first)
      {
        variable_pool.find ("config.cli", string_type); //@@ VAR type

        variable_pool.find ("config.cli.options", strings_type);
        variable_pool.find ("cli.options", strings_type);
      }

      // Configure.
      //

      // config.cli
      //
      if (first)
      {
        auto p (config::required (root, "config.cli", "cli"));

        // If we actually set a new value, test it by trying to execute.
        //
        if (p.second)
        {
          const string& cli (as<string> (p.first));
          const char* args[] = {cli.c_str (), "--version", nullptr};

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
            string l;
            getline (is, l);
            auto p (l.rfind (' '));
            if (p != string::npos)
              ver = string (l, p + 1);

            is.close (); // Don't block the other end.

            if (!pr.wait ())
              throw failed ();

            if (ver.empty ())
              fail << "unexpected output from " << cli;
          }
          catch (const process_error& e)
          {
            error << "unable to execute " << cli << ": " << e.what ();

            if (e.child ())
              exit (1);

            throw failed ();
          }

          if (verb >= 2)
            text << cli << " " << ver;
        }
      }

      // config.cli.options
      //
      // This one is optional. We also merge it into the corresponding
      // cli.* variables. See the cxx module for more information on
      // this merging semantics and some of its tricky aspects.
      //
      if (const value& v = config::optional (root, "config.cli.options"))
        base.assign ("cli.options") += as<strings> (v);
    }
  }
}
