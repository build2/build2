// file      : build/cxx/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/module>

#include <butl/process>
#include <butl/fdstream>

#include <build/scope>
#include <build/diagnostics>

#include <build/config/utility>

#include <build/bin/target>

#include <build/cxx/target>
#include <build/cxx/compile>
#include <build/cxx/link>

using namespace std;
using namespace butl;

namespace build
{
  namespace cxx
  {
    compile compile_;
    link link_;

    extern "C" void
    cxx_init (scope& root,
              scope& base,
              const location& l,
              std::unique_ptr<module>&,
              bool first)
    {
      tracer trace ("cxx::init");
      level4 ([&]{trace << "for " << base.path ();});

      // Initialize the bin module. Only do this if it hasn't already
      // been loaded so that we don't overwrite user's bin.* settings.
      //
      if (base.find_target_type ("obj") == nullptr)
        load_module ("bin", root, base, l);

      // Register target types.
      //
      {
        auto& tts (base.target_types);

        tts.insert<h> ();
        tts.insert<c> ();

        tts.insert<cxx> ();
        tts.insert<hxx> ();
        tts.insert<ixx> ();
        tts.insert<txx> ();
      }

      // Register rules.
      //
      {
        using namespace bin;

        auto& rs (base.rules);

        rs.insert<obja> (default_id, "cxx.compile", compile_);
        rs.insert<obja> (update_id, "cxx.compile", compile_);
        rs.insert<obja> (clean_id, "cxx.compile", compile_);

        rs.insert<objso> (default_id, "cxx.compile", compile_);
        rs.insert<objso> (update_id, "cxx.compile", compile_);
        rs.insert<objso> (clean_id, "cxx.compile", compile_);

        rs.insert<exe> (default_id, "cxx.link", link_);
        rs.insert<exe> (update_id, "cxx.link", link_);
        rs.insert<exe> (clean_id, "cxx.link", link_);

        rs.insert<liba> (default_id, "cxx.link", link_);
        rs.insert<liba> (update_id, "cxx.link", link_);
        rs.insert<liba> (clean_id, "cxx.link", link_);

        rs.insert<libso> (default_id, "cxx.link", link_);
        rs.insert<libso> (update_id, "cxx.link", link_);
        rs.insert<libso> (clean_id, "cxx.link", link_);
      }

      // Configure.
      //

      // config.cxx
      //
      if (first)
      {
        auto r (config::required (root, "config.cxx", "g++"));

        // If we actually set a new value, test it by trying to execute.
        //
        if (r.second)
        {
          const string& cxx (r.first);
          const char* args[] = {cxx.c_str (), "-dumpversion", nullptr};

          if (verb)
            print_process (args);
          else
            text << "test " << cxx;

          string ver;
          try
          {
            process pr (args, 0, -1); // Open pipe to stdout.
            ifdstream is (pr.in_ofd);

            bool r (getline (is, ver));

            if (!r)
              fail << "unexpected output from " << cxx;

            if (!pr.wait ())
              throw failed ();
          }
          catch (const process_error& e)
          {
            error << "unable to execute " << cxx << ": " << e.what ();

            if (e.child ())
              exit (1);

            throw failed ();
          }

          if (verb)
            text << cxx << " " << ver;
        }
      }

      // config.cxx.{p,c,l}options
      // config.cxx.libs
      //
      // These are optional. We also merge them into the corresponding
      // cxx.* variables.
      //
      // The merging part gets a bit tricky if this module has already
      // been loaded in one of the outer scopes. By doing the straight
      // append we would just be repeating the same options over and
      // over. So what we are going to do is only append to a value if
      // it came from this scope. Then the usage for merging becomes:
      //
      // cxx.coptions = <overridable options> # Note: '='.
      // using cxx
      // cxx.coptions += <overriding options> # Note: '+='.
      //
      if (auto* v = config::optional<list_value> (root, "config.cxx.poptions"))
        base.assign ("cxx.poptions") += *v;

      if (auto* v = config::optional<list_value> (root, "config.cxx.coptions"))
        base.assign ("cxx.coptions") += *v;

      if (auto* v = config::optional<list_value> (root, "config.cxx.loptions"))
        base.assign ("cxx.loptions") += *v;

      if (auto* v = config::optional<list_value> (root, "config.cxx.libs"))
        base.assign ("cxx.libs") += *v;
    }
  }
}
