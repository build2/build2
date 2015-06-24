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
#include <build/cxx/module>

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

    void
    init (scope& root, scope& base, const location& l)
    {
      //@@ TODO: avoid multiple inits (generally, for modules).
      //
      tracer trace ("cli::init");

      //@@ Should it be this way?
      //
      if (&root != &base)
        fail (l) << "cli module must be initialized in project root scope";

      // Initialize the cxx module. We need its targets types (?xx{}).
      //
      cxx::init (root, base, l);

      const dir_path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Register our target types.
      //
      target_types.insert (cli::static_type);
      target_types.insert (cli_cxx::static_type);

      // Register our rules.
      //
      rules[default_id][typeid (cli_cxx)].emplace ("cli.compile", compile_);
      rules[update_id][typeid (cli_cxx)].emplace ("cli.compile", compile_);
      rules[clean_id][typeid (cli_cxx)].emplace ("cli.compile", compile_);

      rules[default_id][typeid (cxx::cxx)].emplace ("cli.compile", compile_);
      rules[update_id][typeid (cxx::cxx)].emplace ("cli.compile", compile_);
      rules[clean_id][typeid (cxx::cxx)].emplace ("cli.compile", compile_);

      rules[default_id][typeid (cxx::hxx)].emplace ("cli.compile", compile_);
      rules[update_id][typeid (cxx::hxx)].emplace ("cli.compile", compile_);
      rules[clean_id][typeid (cxx::hxx)].emplace ("cli.compile", compile_);

      rules[default_id][typeid (cxx::ixx)].emplace ("cli.compile", compile_);
      rules[update_id][typeid (cxx::ixx)].emplace ("cli.compile", compile_);
      rules[clean_id][typeid (cxx::ixx)].emplace ("cli.compile", compile_);

      // Configure.
      //

      // config.cli
      //
      {
        auto r (config::required (root, "config.cli", "cli"));

        // If we actually set a new value, test it by trying to execute.
        //
        if (r.second)
        {
          const string& cli (r.first);
          const char* args[] = {cli.c_str (), "--version", nullptr};

          if (verb)
            print_process (args);
          else
            text << "test " << cli;

          string ver;
          try
          {
            process pr (args, false, false, true);
            ifdstream is (pr.in_ofd);

            for (bool first (true); !is.eof (); )
            {
              string l;
              getline (is, l);

              if (first)
              {
                // The version is the last word on the first line.
                //
                auto p (l.rfind (' '));
                if (p != string::npos)
                  ver = string (l, p + 1);

                first = false;
              }
            }

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

          if (verb)
            text << cli << " " << ver;
        }
      }

      // config.cli.options
      //
      // This one is optional. We also merge it into the corresponding
      // cli.* variables.
      //
      if (auto* v = config::optional<list_value> (root, "config.cli.options"))
        root.append ("cli.options") += *v;
    }
  }
}
