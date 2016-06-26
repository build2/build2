// file      : build2/cxx/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/module>

#include <butl/triplet>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/config/utility>
#include <build2/install/utility>

#include <build2/bin/target>

#include <build2/cxx/link>
#include <build2/cxx/guess>
#include <build2/cxx/target>
#include <build2/cxx/compile>
#include <build2/cxx/install>
#include <build2/cxx/utility>

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
              unique_ptr<module_base>&,
              bool first,
              bool,
              const variable_map& config_hints)
    {
      tracer trace ("cxx::init");
      l5 ([&]{trace << "for " << b.out_path ();});

      assert (config_hints.empty ()); // We don't known any hints.

      // Initialize the bin module. Only do this if it hasn't already been
      // loaded so that we don't overwrite user's bin.* settings.
      //
      if (!cast_false<bool> (b["bin.loaded"]))
        load_module ("bin", r, b, loc);

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

        // Note: some overridable, some not.
        //
        v.insert<path>    ("config.cxx",          true);
        v.insert<strings> ("config.cxx.poptions", true);
        v.insert<strings> ("config.cxx.coptions", true);
        v.insert<strings> ("config.cxx.loptions", true);
        v.insert<strings> ("config.cxx.libs",     true);

        v.insert<strings> ("cxx.poptions");
        v.insert<strings> ("cxx.coptions");
        v.insert<strings> ("cxx.loptions");
        v.insert<strings> ("cxx.libs");

        v.insert<strings> ("cxx.export.poptions");
        v.insert<strings> ("cxx.export.coptions");
        v.insert<strings> ("cxx.export.loptions");
        v.insert<strings> ("cxx.export.libs");

        v.insert<string> ("cxx.std",              true);
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
      b.assign ("cxx.poptions") += cast_null<strings> (
        config::optional (r, "config.cxx.poptions"));

      b.assign ("cxx.coptions") += cast_null<strings> (
        config::optional (r, "config.cxx.coptions"));

      b.assign ("cxx.loptions") += cast_null<strings> (
        config::optional (r, "config.cxx.loptions"));

      b.assign ("cxx.libs") += cast_null<strings> (
        config::optional (r, "config.cxx.libs"));

      // config.cxx
      //
      if (first)
      {
        auto p (config::required (r, "config.cxx", path ("g++")));

        // Figure out which compiler we are dealing with, its target, etc.
        //
        const path& cxx (cast<path> (p.first));
        compiler_info ci (guess (cxx, cast_null<strings> (r["cxx.coptions"])));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          //@@ Print project out root or name? Don't print if unnamed?

          text << cxx << ":\n"
               << "  id         " << ci.id << "\n"
               << "  version    " << ci.version.string << "\n"
               << "  major      " << ci.version.major << "\n"
               << "  minor      " << ci.version.minor << "\n"
               << "  patch      " << ci.version.patch << "\n"
               << "  build      " << ci.version.build << "\n"
               << "  signature  " << ci.signature << "\n"
               << "  checksum   " << ci.checksum << "\n"
               << "  target     " << ci.target;
        }

        r.assign<string> ("cxx.id") = ci.id.string ();
        r.assign<string> ("cxx.id.type") = move (ci.id.type);
        r.assign<string> ("cxx.id.variant") = move (ci.id.variant);

        r.assign<string> ("cxx.version") = move (ci.version.string);
        r.assign<uint64_t> ("cxx.version.major") = ci.version.major;
        r.assign<uint64_t> ("cxx.version.minor") = ci.version.minor;
        r.assign<uint64_t> ("cxx.version.patch") = ci.version.patch;
        r.assign<string> ("cxx.version.build") = move (ci.version.build);

        r.assign<string> ("cxx.signature") = move (ci.signature);
        r.assign<string> ("cxx.checksum") = move (ci.checksum);

        // Split/canonicalize the target.
        //

        // Did the user ask us to use config.sub?
        //
        if (ops.config_sub_specified ())
        {
          ci.target = run<string> (ops.config_sub (),
                                   ci.target.c_str (),
                                   [] (string& l) {return move (l);});
          l5 ([&]{trace << "config.sub target: '" << ci.target << "'";});
        }

        try
        {
          string canon;
          triplet t (ci.target, canon);

          l5 ([&]{trace << "canonical target: '" << canon << "'; "
                        << "class: " << t.class_;});

          // Enter as cxx.target.{cpu,vendor,system,version,class}.
          //
          r.assign<string> ("cxx.target") = move (canon);
          r.assign<string> ("cxx.target.cpu") = move (t.cpu);
          r.assign<string> ("cxx.target.vendor") = move (t.vendor);
          r.assign<string> ("cxx.target.system") = move (t.system);
          r.assign<string> ("cxx.target.version") = move (t.version);
          r.assign<string> ("cxx.target.class") = move (t.class_);
        }
        catch (const invalid_argument& e)
        {
          // This is where we could suggest that the user specifies
          // --config-sub to help us out.
          //
          fail << "unable to parse compiler target '" << ci.target << "': "
               << e.what () <<
            info << "consider using the --config-sub option";
        }
      }

      // Configure "installability" of our target types.
      //
      using namespace install;

      install_path<hxx> (b, dir_path ("include")); // Into install.include.
      install_path<ixx> (b, dir_path ("include"));
      install_path<txx> (b, dir_path ("include"));
      install_path<h>   (b, dir_path ("include"));

      // Create additional target types for certain target platforms.
      //
      const string& tclass (cast<string> (r["cxx.target.class"]));

      if (tclass == "windows")
      {
        const target_type& dll (b.derive_target_type<file> ("dll").first);
        install_path (dll, b, dir_path ("bin"));
      }

      return true;
    }
  }
}
