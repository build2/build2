// file      : build2/cxx/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/module>

#include <butl/process>
#include <butl/fdstream>

#include <build2/scope>
#include <build2/diagnostics>

#include <build2/config/utility>
#include <build2/install/utility>

#include <build2/bin/target>

#include <build2/cxx/target>
#include <build2/cxx/compile>
#include <build2/cxx/link>
#include <build2/cxx/install>

using namespace std;
using namespace butl;

namespace build2
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
        auto& v (var_pool);

        v.find ("config.cxx", string_type); //@@ VAR type

        v.find ("config.cxx.poptions", strings_type);
        v.find ("config.cxx.coptions", strings_type);
        v.find ("config.cxx.loptions", strings_type);
        v.find ("config.cxx.libs", strings_type);

        v.find ("cxx.poptions", strings_type);
        v.find ("cxx.coptions", strings_type);
        v.find ("cxx.loptions", strings_type);
        v.find ("cxx.libs", strings_type);

        v.find ("cxx.export.poptions", strings_type);
        v.find ("cxx.export.coptions", strings_type);
        v.find ("cxx.export.loptions", strings_type);
        v.find ("cxx.export.libs", strings_type);

        v.find ("cxx.std", string_type);
      }

      // Register target types.
      //
      {
        auto& t (b.target_types);

        t.insert<h> ();
        t.insert<c> ();

        t.insert<cxx> ();
        t.insert<hxx> ();
        t.insert<ixx> ();
        t.insert<txx> ();
      }

      // Register rules.
      //
      {
        using namespace bin;

        auto& r (b.rules);

        r.insert<obja> (perform_update_id, "cxx.compile", compile::instance);

        r.insert<obja> (perform_update_id, "cxx.compile", compile::instance);
        r.insert<obja> (perform_clean_id, "cxx.compile", compile::instance);

        r.insert<objso> (perform_update_id, "cxx.compile", compile::instance);
        r.insert<objso> (perform_clean_id, "cxx.compile", compile::instance);

        r.insert<exe> (perform_update_id, "cxx.link", link::instance);
        r.insert<exe> (perform_clean_id, "cxx.link", link::instance);

        r.insert<liba> (perform_update_id, "cxx.link", link::instance);
        r.insert<liba> (perform_clean_id, "cxx.link", link::instance);

        r.insert<libso> (perform_update_id, "cxx.link", link::instance);
        r.insert<libso> (perform_clean_id, "cxx.link", link::instance);

        // Register for configure so that we detect unresolved imports
        // during configuration rather that later, e.g., during update.
        //
        r.insert<obja> (configure_update_id, "cxx.compile", compile::instance);
        r.insert<objso> (configure_update_id, "cxx.compile", compile::instance);

        r.insert<exe> (configure_update_id, "cxx.link", link::instance);
        r.insert<liba> (configure_update_id, "cxx.link", link::instance);
        r.insert<libso> (configure_update_id, "cxx.link", link::instance);

        //@@ Should we check if install module was loaded (see bin)?
        //
        r.insert<exe> (perform_install_id, "cxx.install", install::instance);
        r.insert<liba> (perform_install_id, "cxx.install", install::instance);
        r.insert<libso> (perform_install_id, "cxx.install", install::instance);
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
        using build2::install::path;

        path<hxx> (b, dir_path ("include")); // Install into install.include.
        path<ixx> (b, dir_path ("include"));
        path<txx> (b, dir_path ("include"));
        path<h>   (b, dir_path ("include"));
      }

      return true;
    }
  }
}
