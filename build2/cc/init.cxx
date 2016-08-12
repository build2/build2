// file      : build2/cc/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/init>

#include <butl/triplet>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/config/utility>

#include <build2/cc/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    bool
    vars_init (scope& r,
               scope&,
               const location&,
               unique_ptr<module_base>&,
               bool first,
               bool,
               const variable_map&)
    {
      tracer trace ("cc::vars_init");
      l5 ([&]{trace << "for " << r.out_path ();});

      assert (first);

      // Enter variables. Note: some overridable, some not.
      //
      auto& v (var_pool);

      v.insert<strings> ("config.cc.poptions", true);
      v.insert<strings> ("config.cc.coptions", true);
      v.insert<strings> ("config.cc.loptions", true);
      v.insert<strings> ("config.cc.libs",     true);

      v.insert<strings> ("cc.poptions");
      v.insert<strings> ("cc.coptions");
      v.insert<strings> ("cc.loptions");
      v.insert<strings> ("cc.libs");

      v.insert<strings> ("cc.export.poptions");
      v.insert<strings> ("cc.export.coptions");
      v.insert<strings> ("cc.export.loptions");
      v.insert<strings> ("cc.export.libs");

      // Hint variables (not overridable).
      //
      v.insert<string> ("config.cc.id");
      v.insert<string> ("config.cc.target");
      v.insert<string> ("config.cc.pattern");

      return true;
    }

    bool
    config_init (scope& r,
                 scope& b,
                 const location& loc,
                 unique_ptr<module_base>&,
                 bool first,
                 bool,
                 const variable_map& hints)
    {
      tracer trace ("cc::config_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Load cc.vars.
      //
      if (first)
      {
        if (!cast_false<bool> (b["cc.vars.loaded"]))
          load_module ("cc.vars", r, b, loc);
      }

      // Configure.
      //
      if (first)
      {
        // Adjust module priority (compiler).
        //
        config::save_module (r, "cc", 250);

        // config.cc.id
        //
        {
          // This value must be hinted.
          //
          r.assign<string> ("cc.id") = cast<string> (hints["config.cc.id"]);
        }

        // config.cc.target
        //
        {
          // This value must be hinted and already canonicalized.
          //
          const string& s (cast<string> (hints["config.cc.target"]));

          try
          {
            //@@ We do it in the hinting module and here. Any way not to
            //   duplicate the effort? Maybe move the splitting here and
            //   simply duplicate the values there?
            //
            triplet t (s);

            // Enter as cc.target.{cpu,vendor,system,version,class}.
            //
            r.assign<string> ("cc.target") = s;
            r.assign<string> ("cc.target.cpu") = move (t.cpu);
            r.assign<string> ("cc.target.vendor") = move (t.vendor);
            r.assign<string> ("cc.target.system") = move (t.system);
            r.assign<string> ("cc.target.version") = move (t.version);
            r.assign<string> ("cc.target.class") = move (t.class_);
          }
          catch (const invalid_argument& e)
          {
            assert (false); // Should have been caught by the hinting module.
          }
        }

        // config.cc.pattern
        //
        {
          // This value could be hinted.
          //
          if (auto l = hints["config.cc.pattern"])
            r.assign<string> ("cc.pattern") = cast<string> (l);
        }

        // Note that we are not having a config report since it will just
        // duplicate what has already been printed by the hinting module.
      }

      // config.cc.{p,c,l}options
      // config.cc.libs
      //
      // @@ Same nonsense as in module.
      //
      //
      b.assign ("cc.poptions") += cast_null<strings> (
        config::optional (r, "config.cc.poptions"));

      b.assign ("cc.coptions") += cast_null<strings> (
        config::optional (r, "config.cc.coptions"));

      b.assign ("cc.loptions") += cast_null<strings> (
        config::optional (r, "config.cc.loptions"));

      b.assign ("cc.libs") += cast_null<strings> (
        config::optional (r, "config.cc.libs"));

      // Load the bin.config module.
      //
      if (!cast_false<bool> (b["bin.config.loaded"]))
      {
        // Prepare configuration hints. They are only used on the first load
        // of bin.config so we only populate them on our first load.
        //
        variable_map h;
        if (first)
        {
          h.assign ("config.bin.target") = cast<string> (r["cc.target"]);

          if (auto l = hints["config.bin.pattern"])
            h.assign ("config.bin.pattern") = cast<string> (l);
        }

        load_module ("bin.config", r, b, loc, false, h);
      }

      // Verify bin's target matches ours (we do it even if we loaded it
      // ourselves since the target can come from the configuration and not
      // our hint).
      //
      if (first)
      {
        const string& ct (cast<string> (r["cc.target"]));
        const string& bt (cast<string> (r["bin.target"]));

        if (bt != ct)
          fail (loc) << "cc and bin module target mismatch" <<
            info << "cc.target is " << ct <<
            info << "bin.target is " << bt;
      }

      const string& cid (cast<string> (r["cc.id"]));
      const string& tsys (cast<string> (r["cc.target.system"]));

      // Load bin.*.config for bin.* modules we may need (see core_init()
      // below).
      //
      if (auto l = r["config.bin.lib"])
      {
        if (cast<string> (l) != "shared")
        {
          if (!cast_false<bool> (b["bin.ar.config.loaded"]))
            load_module ("bin.ar.config", r, b, loc);
        }
      }

      if (cid == "msvc")
      {
        if (!cast_false<bool> (b["bin.ld.config.loaded"]))
          load_module ("bin.ld.config", r, b, loc);
      }

      if (tsys == "mingw32")
      {
        if (!cast_false<bool> (b["bin.rc.config.loaded"]))
          load_module ("bin.rc.config", r, b, loc);
      }

      return true;
    }

    bool
    core_init (scope& r,
               scope& b,
               const location& loc,
               unique_ptr<module_base>&,
               bool,
               bool,
               const variable_map& hints)
    {
      tracer trace ("cc::core_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Load cc.config.
      //
      if (!cast_false<bool> (b["cc.config.loaded"]))
        load_module ("cc.config", r, b, loc, false, hints);

      // Load the bin module.
      //
      if (!cast_false<bool> (b["bin.loaded"]))
        load_module ("bin", r, b, loc);

      const string& cid (cast<string> (r["cc.id"]));
      const string& tsys (cast<string> (r["cc.target.system"]));

      // Load the bin.ar module unless we were asked to only build shared
      // libraries.
      //
      if (auto l = r["config.bin.lib"])
      {
        if (cast<string> (l) != "shared")
        {
          if (!cast_false<bool> (b["bin.ar.loaded"]))
            load_module ("bin.ar", r, b, loc);
        }
      }

      // In the VC world you link things directly with link.exe so load the
      // bin.ld module.
      //
      if (cid == "msvc")
      {
        if (!cast_false<bool> (b["bin.ld.loaded"]))
          load_module ("bin.ld", r, b, loc);
      }

      // If our target is MinGW, then we will need the resource compiler
      // (windres) in order to embed manifests into executables.
      //
      if (tsys == "mingw32")
      {
        if (!cast_false<bool> (b["bin.rc.loaded"]))
          load_module ("bin.rc", r, b, loc);
      }

      return true;
    }

    bool
    init (scope& r,
          scope& b,
          const location& loc,
          unique_ptr<module_base>&,
          bool,
          bool,
          const variable_map&)
    {
      tracer trace ("cc::init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // This module is an "alias" for c.config and cxx.config. Its intended
      // use is to make sure that the C/C++ configuration is captured in an
      // amalgamation rather than subprojects.
      //
      // We want to order the loading to match what user specified on the
      // command line (config.c or config.cxx). This way the first loaded
      // module (with user-specified config.*) will hint the compiler to the
      // second.
      //
      bool lc (!cast_false<bool> (b["c.config.loaded"]));
      bool lp (!cast_false<bool> (b["cxx.config.loaded"]));

      // If none of them are already loaded, load c first only if config.c
      // is specified.
      //
      if (lc && lp && r["config.c"])
      {
        load_module ("c.config", r, b, loc);
        load_module ("cxx.config", r, b, loc);
      }
      else
      {
        if (lp) load_module ("cxx.config", r, b, loc);
        if (lc) load_module ("c.config", r, b, loc);
      }

      return true;
    }
  }
}
