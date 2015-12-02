// file      : build/cxx/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/module>

#include <butl/process>
#include <butl/fdstream>

#include <build/scope>
#include <build/diagnostics>

#include <build/config/utility>
#include <build/install/utility>

#include <build/bin/target>

#include <build/cxx/target>
#include <build/cxx/compile>
#include <build/cxx/link>
#include <build/cxx/install>

using namespace std;
using namespace butl;

namespace build
{
  namespace cxx
  {
    extern "C" bool
    cxx_init (scope& r,
              scope& b,
              const location& loc,
              std::unique_ptr<module>&,
              bool first,
              bool)
    {
      tracer trace ("cxx::init");
      level5 ([&]{trace << "for " << b.out_path ();});

      // Initialize the bin module. Only do this if it hasn't already
      // been loaded so that we don't overwrite user's bin.* settings.
      //
      {
        auto l (b["bin.loaded"]);

        if (!l || !as<bool> (*l))
          load_module (false, "bin", r, b, loc);
      }

      // Register target types.
      //
      {
        auto& tts (b.target_types);

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

        auto& rs (b.rules);

        rs.insert<obja> (perform_id, update_id, "cxx", compile::instance);
        rs.insert<obja> (perform_id, clean_id, "cxx", compile::instance);

        rs.insert<objso> (perform_id, update_id, "cxx", compile::instance);
        rs.insert<objso> (perform_id, clean_id, "cxx", compile::instance);

        rs.insert<exe> (perform_id, update_id, "cxx", link::instance);
        rs.insert<exe> (perform_id, clean_id, "cxx", link::instance);

        rs.insert<liba> (perform_id, update_id, "cxx", link::instance);
        rs.insert<liba> (perform_id, clean_id, "cxx", link::instance);

        rs.insert<libso> (perform_id, update_id, "cxx", link::instance);
        rs.insert<libso> (perform_id, clean_id, "cxx", link::instance);

        // Register for configure so that we detect unresolved imports
        // during configuration rather that later, e.g., during update.
        //
        rs.insert<obja> (configure_id, update_id, "cxx", compile::instance);
        rs.insert<objso> (configure_id, update_id, "cxx", compile::instance);
        rs.insert<exe> (configure_id, update_id, "cxx", link::instance);
        rs.insert<liba> (configure_id, update_id, "cxx", link::instance);
        rs.insert<libso> (configure_id, update_id, "cxx", link::instance);

        //@@ Should we check if install module was loaded (see bin)?
        //
        rs.insert<exe> (perform_id, install_id, "cxx", install::instance);
        rs.insert<liba> (perform_id, install_id, "cxx", install::instance);
        rs.insert<libso> (perform_id, install_id, "cxx", install::instance);
      }

      // Enter module variables.
      //
      // @@ Probably should only be done on load; make sure reset() unloads
      //    modules.
      //
      // @@ Should probably cache the variable pointers so we don't have
      //    to keep looking them up.
      //
      if (first)
      {
        var_pool.find ("config.cxx", string_type); //@@ VAR type

        var_pool.find ("config.cxx.poptions", strings_type);
        var_pool.find ("config.cxx.coptions", strings_type);
        var_pool.find ("config.cxx.loptions", strings_type);
        var_pool.find ("config.cxx.libs", strings_type);

        var_pool.find ("cxx.poptions", strings_type);
        var_pool.find ("cxx.coptions", strings_type);
        var_pool.find ("cxx.loptions", strings_type);
        var_pool.find ("cxx.libs", strings_type);

        var_pool.find ("cxx.export.poptions", strings_type);
        var_pool.find ("cxx.export.coptions", strings_type);
        var_pool.find ("cxx.export.loptions", strings_type);
        var_pool.find ("cxx.export.libs", strings_type);

        var_pool.find ("cxx.std", string_type);

        var_pool.find ("h.ext", string_type);
        var_pool.find ("c.ext", string_type);
        var_pool.find ("hxx.ext", string_type);
        var_pool.find ("ixx.ext", string_type);
        var_pool.find ("txx.ext", string_type);
        var_pool.find ("cxx.ext", string_type);
      }

      // Configure.
      //

      // config.cxx
      //
      if (first)
      {
        auto p (config::required (r, "config.cxx", "g++"));

        // If we actually set a new value, test it by trying to execute.
        //
        if (p.second)
        {
          const string& cxx (as<string> (p.first));
          const char* args[] = {cxx.c_str (), "-dumpversion", nullptr};

          if (verb >= 2)
            print_process (args);
          else if (verb)
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

          if (verb >= 2)
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
      if (const value& v = config::optional (r, "config.cxx.poptions"))
        b.assign ("cxx.poptions") += as<strings> (v);

      if (const value& v = config::optional (r, "config.cxx.coptions"))
        b.assign ("cxx.coptions") += as<strings> (v);

      if (const value& v = config::optional (r, "config.cxx.loptions"))
        b.assign ("cxx.loptions") += as<strings> (v);

      if (const value& v = config::optional (r, "config.cxx.libs"))
        b.assign ("cxx.libs") += as<strings> (v);

      // Configure "installability" of our target types.
      //
      {
        using build::install::path;

        path<hxx> (b, dir_path ("include")); // Install into install.include.
        path<ixx> (b, dir_path ("include"));
        path<txx> (b, dir_path ("include"));
        path<h>   (b, dir_path ("include"));
      }

      return true;
    }
  }
}
