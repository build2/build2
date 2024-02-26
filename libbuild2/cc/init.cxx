// file      : libbuild2/cc/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/init.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/cc/target.hxx>
#include <libbuild2/cc/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    // Scope operation callback that cleans up module sidebuilds.
    //
    static target_state
    clean_module_sidebuilds (action, const scope& rs, const dir&)
    {
      context& ctx (rs.ctx);

      const dir_path& out_root (rs.out_path ());

      dir_path d (out_root /
                  rs.root_extra->build_dir /
                  module_build_modules_dir);

      if (exists (d))
      {
        if (rmdir_r (ctx, d))
        {
          // Clean up cc/build/ if it became empty.
          //
          d = out_root / rs.root_extra->build_dir / module_build_dir;
          if (empty (d))
          {
            rmdir (ctx, d, 2);

            // Clean up cc/ if it became empty.
            //
            d = out_root / rs.root_extra->build_dir / module_dir;
            if (empty (d))
            {
              rmdir (ctx, d, 2);

              // And build/ if it also became empty (e.g., in case of a build
              // with a transient configuration).
              //
              d = out_root / rs.root_extra->build_dir;
              if (empty (d))
                rmdir (ctx, d, 2);
            }
          }

          return target_state::changed;
        }
      }

      return target_state::unchanged;
    }

    bool
    core_vars_init (scope& rs,
                    scope&,
                    const location& loc,
                    bool first,
                    bool,
                    module_init_extra&)
    {
      tracer trace ("cc::core_vars_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      // Load bin.vars (we need its config.bin.target/pattern for hints).
      //
      load_module (rs, rs, "bin.vars", loc);

      // Enter variables.
      //
      // All the variables we enter are qualified so go straight for the
      // public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));

      auto v_t (variable_visibility::target);

      // NOTE: remember to update documentation if changing anything here.
      //
      vp.insert<strings> ("config.cc.poptions");
      vp.insert<strings> ("config.cc.coptions");
      vp.insert<strings> ("config.cc.loptions");
      vp.insert<strings> ("config.cc.aoptions");
      vp.insert<strings> ("config.cc.libs");

      vp.insert<string> ("config.cc.internal.scope");

      vp.insert<bool> ("config.cc.reprocess"); // See cc.preprocess below.

      vp.insert<abs_dir_path> ("config.cc.pkgconfig.sysroot");

      vp.insert<strings> ("cc.poptions");
      vp.insert<strings> ("cc.coptions");
      vp.insert<strings> ("cc.loptions");
      vp.insert<strings> ("cc.aoptions");
      vp.insert<strings> ("cc.libs");

      vp.insert<string>  ("cc.internal.scope");
      vp.insert<strings> ("cc.internal.libs");

      vp.insert<strings>      ("cc.export.poptions");
      vp.insert<strings>      ("cc.export.coptions");
      vp.insert<strings>      ("cc.export.loptions");
      vp.insert<vector<name>> ("cc.export.libs");
      vp.insert<vector<name>> ("cc.export.impl_libs");

      // Header (-I) and library (-L) search paths to use in the generated .pc
      // files instead of the default install.{include,lib}. Relative paths
      // are resolved as install paths.
      //
      vp.insert<dir_paths> ("cc.pkgconfig.include");
      vp.insert<dir_paths> ("cc.pkgconfig.lib");

      // Hint variables (not overridable).
      //
      vp.insert<string>         ("config.cc.id",      false);
      vp.insert<string>         ("config.cc.hinter",  false); // Hinting module.
      vp.insert<string>         ("config.cc.pattern", false);
      vp.insert<strings>        ("config.cc.mode",    false);
      vp.insert<target_triplet> ("config.cc.target",  false);

      // Compiler runtime and C standard library.
      //
      vp.insert<string> ("cc.runtime");
      vp.insert<string> ("cc.stdlib");

      // Library target type in the <lang>[,<type>...] form where <lang> is
      // "c" (C library), "cxx" (C++ library), or "cc" (C-common library but
      // the specific language is not known). Currently recognized <type>
      // values are "binless" (library is binless) and "recursively-binless"
      // (library and all its prerequisite libraries are binless). Note that
      // another indication of a binless library is an empty path, which could
      // be easier/faster to check. Note also that there should be no
      // whitespaces of any kind and <lang> is always first.
      //
      // This value should be set on the library target as a rule-specific
      // variable by the matching rule. It is also saved in the generated
      // pkg-config files. Currently <lang> is used to decide which *.libs to
      // use during static linking. The "cc" language is used in the import
      // installed logic.
      //
      // Note that this variable cannot be set via the target type/pattern-
      // specific mechanism (see process_libraries()).
      //
      vp.insert<string> ("cc.type", v_t);

      // If set and is true, then this (imported) library has been found in a
      // system library search directory.
      //
      vp.insert<bool> ("cc.system", v_t);

      // C++ module name. Set on the bmi*{} target as a rule-specific variable
      // by the matching rule. Can also be set by the user (normally via the
      // x.module_name alias) on the x_mod{} source.
      //
      vp.insert<string> ("cc.module_name", v_t);

      // Importable header marker (normally set via the x.importable alias).
      //
      // Note that while at first it might seem like a good idea to allow
      // setting it on a scope, that will cause translation of inline/template
      // includes which is something we definitely don't want.
      //
      vp.insert<bool> ("cc.importable", v_t);

      // Ability to disable using preprocessed output for compilation.
      //
      vp.insert<bool> ("cc.reprocess");

      // Execute serially with regards to any other recipe. This is primarily
      // useful when compiling large translation units or linking large
      // binaries that require so much memory that doing that in parallel with
      // other compilation/linking jobs is likely to summon the OOM killer.
      //
      vp.insert<bool> ("cc.serialize");

      // Register scope operation callback.
      //
      // It feels natural to clean up sidebuilds as a post operation but that
      // prevents the (otherwise-empty) out root directory to be cleaned up
      // (via the standard fsdir{} chain).
      //
      rs.operation_callbacks.emplace (
        perform_clean_id,
        scope::operation_callback {&clean_module_sidebuilds, nullptr /*post*/});

      return true;
    }

    bool
    core_guess_init (scope& rs,
                     scope&,
                     const location& loc,
                     bool first,
                     bool,
                     module_init_extra& extra)
    {
      tracer trace ("cc::core_guess_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      auto& h (extra.hints);

      // Load cc.core.vars.
      //
      load_module (rs, rs, "cc.core.vars", loc);

      // config.cc.{id,hinter}
      //
      // These values must be hinted.
      //
      {
        rs.assign<string> ("cc.id") = cast<string> (h["config.cc.id"]);
        rs.assign<string> ("cc.hinter") = cast<string> (h["config.cc.hinter"]);
      }

      // config.cc.target
      //
      // This value must be hinted.
      //
      {
        const auto& t (cast<target_triplet> (h["config.cc.target"]));

        // Also enter as cc.target.{cpu,vendor,system,version,class} for
        // convenience of access.
        //
        rs.assign<string> ("cc.target.cpu")     = t.cpu;
        rs.assign<string> ("cc.target.vendor")  = t.vendor;
        rs.assign<string> ("cc.target.system")  = t.system;
        rs.assign<string> ("cc.target.version") = t.version;
        rs.assign<string> ("cc.target.class")   = t.class_;

        rs.assign<target_triplet> ("cc.target") = t;
      }

      // config.cc.pattern
      //
      // This value could be hinted. Note that the hints may not be the same.
      //
      {
        rs.assign<string> ("cc.pattern") =
          cast_empty<string> (h["config.cc.pattern"]);
      }

      // config.cc.mode
      //
      // This value could be hinted. Note that the hints may not be the same.
      //
      {
        rs.assign<strings> ("cc.mode") =
          cast_empty<strings> (h["config.cc.mode"]);
      }

      // cc.runtime
      // cc.stdlib
      //
      rs.assign ("cc.runtime") = cast<string> (h["cc.runtime"]);
      rs.assign ("cc.stdlib") = cast<string> (h["cc.stdlib"]);

      return true;
    }

    bool
    core_config_init (scope& rs,
                      scope&,
                      const location& loc,
                      bool first,
                      bool,
                      module_init_extra& extra)
    {
      tracer trace ("cc::core_config_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      // Load cc.core.guess.
      //
      load_module (rs, rs, "cc.core.guess", loc);

      // Configuration.
      //
      using config::lookup_config;

      // Adjust module priority (compiler).
      //
      config::save_module (rs, "cc", 250);

      // Note that we are not having a config report since it will just
      // duplicate what has already been printed by the hinting module.

      // config.cc.{p,c,l}options
      // config.cc.libs
      //
      // @@ Same nonsense as in module.
      //
      //
      rs.assign ("cc.poptions") += cast_null<strings> (
        lookup_config (rs, "config.cc.poptions", nullptr));

      rs.assign ("cc.coptions") += cast_null<strings> (
        lookup_config (rs, "config.cc.coptions", nullptr));

      rs.assign ("cc.loptions") += cast_null<strings> (
        lookup_config (rs, "config.cc.loptions", nullptr));

      rs.assign ("cc.aoptions") += cast_null<strings> (
        lookup_config (rs, "config.cc.aoptions", nullptr));

      rs.assign ("cc.libs") += cast_null<strings> (
        lookup_config (rs, "config.cc.libs", nullptr));

      // config.cc.internal.scope
      //
      // Note: save omitted.
      //
      if (lookup l = lookup_config (rs, "config.cc.internal.scope"))
      {
        if (cast<string> (l) == "current")
          fail << "'current' value in config.cc.internal.scope";

        // This is necessary in case we are acting as bundle amalgamation.
        //
        rs.assign ("cc.internal.scope") = *l;
      }

      // config.cc.reprocess
      //
      // Note: save omitted.
      //
      if (lookup l = lookup_config (rs, "config.cc.reprocess"))
        rs.assign ("cc.reprocess") = *l;

      // config.cc.pkgconfig.sysroot
      //
      // Let's look it up instead of just marking for saving to make sure the
      // path is valid.
      //
      // Note: save omitted.
      //
      lookup_config (rs, "config.cc.pkgconfig.sysroot");

      // Load the bin.config module.
      //
      if (!cast_false<bool> (rs["bin.config.loaded"]))
      {
        // Prepare configuration hints (pretend it belongs to root scope).
        // They are only used on the first load of bin.config so we only
        // populate them on our first load.
        //
        variable_map h (rs);

        if (first)
        {
          // Note that all these variables have already been registered.
          //
          h.assign ("config.bin.target") =
            cast<target_triplet> (rs["cc.target"]).representation ();

          if (auto l = extra.hints["config.bin.pattern"])
            h.assign ("config.bin.pattern") = cast<string> (l);
        }

        init_module (rs, rs, "bin.config", loc, false /* optional */, h);
      }

      // Verify bin's target matches ours (we do it even if we loaded it
      // ourselves since the target can come from the configuration and not
      // our hint).
      //
      if (first)
      {
        const auto& ct (cast<target_triplet> (rs["cc.target"]));
        const auto& bt (cast<target_triplet> (rs["bin.target"]));

        if (bt != ct)
        {
          const auto& h (cast<string> (rs["cc.hinter"]));

          fail (loc) << h << " and bin module target mismatch" <<
            info << h << " target is " << ct <<
            info << "bin target is " << bt;
        }
      }

      // Load bin.* modules we may need (see core_init() below).
      //
      const string& tsys (cast<string> (rs["cc.target.system"]));

      load_module (rs, rs, "bin.ar.config", loc);

      if (tsys == "win32-msvc")
      {
        load_module (rs, rs, "bin.ld.config", loc);
        load_module (rs, rs, "bin.def", loc);
      }

      if (tsys == "mingw32")
        load_module (rs, rs, "bin.rc.config", loc);

      return true;
    }

    bool
    core_init (scope& rs,
               scope&,
               const location& loc,
               bool first,
               bool,
               module_init_extra& extra)
    {
      tracer trace ("cc::core_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      const string& tsys (cast<string> (rs["cc.target.system"]));

      // Load cc.core.config.
      //
      load_module (rs, rs, "cc.core.config", loc, extra.hints);

      // Load the bin module.
      //
      load_module (rs, rs, "bin", loc);

      // Load the bin.ar module.
      //
      load_module (rs, rs, "bin.ar", loc);

      // For this target we link things directly with link.exe so load the
      // bin.ld module.
      //
      if (tsys == "win32-msvc")
        load_module (rs, rs, "bin.ld", loc);

      // If our target is MinGW, then we will need the resource compiler
      // (windres) in order to embed manifests into executables.
      //
      if (tsys == "mingw32")
        load_module (rs, rs, "bin.rc", loc);

      return true;
    }

    // The cc module is an "alias" for c and cxx. Its intended use is to make
    // sure that the C/C++ configuration is captured in an amalgamation rather
    // than subprojects.
    //
    static inline bool
    init_alias (tracer& trace,
                scope& rs,
                scope& bs,
                const char* m,
                const char* c,
                const char* c_loaded,
                const char* cxx,
                const char* cxx_loaded,
                const location& loc,
                const variable_map& hints)
    {
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << m << " module must be loaded in project root";

      // We want to order the loading to match what user specified on the
      // command line (config.c or config.cxx). This way the first loaded
      // module (with user-specified config.*) will hint the compiler to the
      // second.
      //
      bool lc (!cast_false<bool> (rs[c_loaded]));
      bool lp (!cast_false<bool> (rs[cxx_loaded]));

      // If none of them are already loaded, load c first only if config.c
      // is specified.
      //
      if (lc && lp && rs["config.c"])
      {
        init_module (rs, rs, c,   loc, false /* optional */, hints);
        init_module (rs, rs, cxx, loc, false /* optional */, hints);
      }
      else
      {
        if (lp) init_module (rs, rs, cxx, loc, false, hints);
        if (lc) init_module (rs, rs, c,   loc, false, hints);
      }

      return true;
    }

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 bool,
                 bool,
                 module_init_extra& extra)
    {
      tracer trace ("cc::config_init");
      return init_alias (trace, rs, bs,
                         "cc.config",
                         "c.config",   "c.config.loaded",
                         "cxx.config", "cxx.config.loaded",
                         loc, extra.hints);
    }

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          bool,
          bool,
          module_init_extra& extra)
    {
      tracer trace ("cc::init");
      return init_alias (trace, rs, bs,
                         "cc",
                         "c",   "c.loaded",
                         "cxx", "cxx.loaded",
                         loc, extra.hints);
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"cc.core.vars",   nullptr, core_vars_init},
      {"cc.core.guess",  nullptr, core_guess_init},
      {"cc.core.config", nullptr, core_config_init},
      {"cc.core",        nullptr, core_init},
      {"cc.config",      nullptr, config_init},
      {"cc",             nullptr, init},
      {nullptr,          nullptr, nullptr}
    };

    const module_functions*
    build2_cc_load ()
    {
      return mod_functions;
    }
  }
}
