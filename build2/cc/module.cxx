// file      : build2/cc/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/module>

#include <iomanip> // left, setw()

#include <butl/triplet>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/bin/target>

#include <build2/config/utility>
#include <build2/install/utility>

#include <build2/cc/guess>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    void config_module::
    init (scope& rs, const location& loc, const variable_map&)
    {
      tracer trace (x, "config_init");

      bool cc_loaded (cast_false<bool> (rs["cc.core.config.loaded"]));

      // Configure.
      //
      compiler_info ci; // For program patterns.

      // Adjust module priority (compiler). Also order cc module before us
      // (we don't want to use priorities for that in case someone manages
      // to slot in-between).
      //
      if (!cc_loaded)
        config::save_module (rs, "cc", 250);

      config::save_module (rs, x, 250);

      const variable& config_c_coptions (var_pool["config.cc.coptions"]);

      // config.x
      //

      // Normally we will have a persistent configuration and computing the
      // default value every time will be a waste. So try without a default
      // first.
      //
      auto p (config::omitted (rs, config_x));

      if (p.first == nullptr)
      {
        // If someone already loaded cc.core.config then use its toolchain
        // id and (optional) pattern to guess an appropriate default (e.g.,
        // for {gcc, *-4.9} we will get g++-4.9).
        //
        path d (cc_loaded
                ? guess_default (x_lang,
                                 cast<string> (rs["cc.id"]),
                                 cast_null<string> (rs["cc.pattern"]))
                : path (x_default));

        // If this value was hinted, save it as commented out so that if the
        // user changes the source of the pattern, this one will get updated
        // as well.
        //
        auto p1 (config::required (rs,
                                   config_x,
                                   d,
                                   false,
                                   cc_loaded ? config::save_commented : 0));
        p.first = &p1.first.get ();
        p.second = p1.second;
      }

      // Figure out which compiler we are dealing with, its target, etc.
      //
      const path& xc (cast<path> (*p.first));
      ci = guess (x_lang,
                  xc,
                  cast_null<strings> (rs[config_c_coptions]),
                  cast_null<strings> (rs[config_x_coptions]));

      // Translate x_std value (if any) to the compiler option (if any).
      //
      if (auto l = rs[x_std])
        tstd = translate_std (ci, rs, cast<string> (*l));

      // Extract system library search paths from the compiler.
      //
      dir_paths lib_dirs (ci.id.type == "msvc"
                          ? msvc_library_search_paths (ci.path, rs)
                          : gcc_library_search_paths (ci.path, rs));

      // If this is a new value (e.g., we are configuring), then print the
      // report at verbosity level 2 and up (-v).
      //
      if (verb >= (p.second ? 2 : 3))
      {
        diag_record dr (text);

        {
          dr << x << ' ' << project (rs) << '@' << rs.out_path () << '\n'
             << "  " << left << setw (11) << x << ci.path << '\n'
             << "  id         " << ci.id << '\n'
             << "  version    " << ci.version.string << '\n'
             << "  major      " << ci.version.major << '\n'
             << "  minor      " << ci.version.minor << '\n'
             << "  patch      " << ci.version.patch << '\n';
        }

        if (!ci.version.build.empty ())
        {
          dr << "  build      " << ci.version.build << '\n';
        }

        {
          dr << "  signature  " << ci.signature << '\n'
             << "  target     " << ci.target << '\n';
        }

        if (!tstd.empty ())
        {
          dr << "  std        " << tstd << '\n';
        }

        if (!ci.cc_pattern.empty ()) // bin_pattern printed by bin
        {
          dr << "  pattern    " << ci.cc_pattern << '\n';
        }

        if (verb >= 3 && !lib_dirs.empty ())
        {
          dr << "  lib dirs\n";
          for (const dir_path& d: lib_dirs)
            dr << "    " << d << '\n';
        }

        {
          dr << "  checksum   " << ci.checksum;
        }
      }

      rs.assign (x_path) = move (ci.path);
      rs.assign (x_sys_lib_dirs) = move (lib_dirs);

      rs.assign (x_id) = ci.id.string ();
      rs.assign (x_id_type) = move (ci.id.type);
      rs.assign (x_id_variant) = move (ci.id.variant);

      rs.assign (x_version) = move (ci.version.string);
      rs.assign (x_version_major) = ci.version.major;
      rs.assign (x_version_minor) = ci.version.minor;
      rs.assign (x_version_patch) = ci.version.patch;
      rs.assign (x_version_build) = move (ci.version.build);

      rs.assign (x_signature) = move (ci.signature);
      rs.assign (x_checksum) = move (ci.checksum);

      // Split/canonicalize the target. First see if the user asked us to
      // use config.sub.
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

        // Enter as x.target.{cpu,vendor,system,version,class}.
        //
        rs.assign (x_target) = move (canon);
        rs.assign (x_target_cpu) = move (t.cpu);
        rs.assign (x_target_vendor) = move (t.vendor);
        rs.assign (x_target_system) = move (t.system);
        rs.assign (x_target_version) = move (t.version);
        rs.assign (x_target_class) = move (t.class_);
      }
      catch (const invalid_argument& e)
      {
        // This is where we suggest that the user specifies --config-sub to
        // help us out.
        //
        fail << "unable to parse " << x_lang << " compiler target '"
             << ci.target << "': " << e.what () <<
          info << "consider using the --config-sub option";
      }

      // config.x.{p,c,l}options
      // config.x.libs
      //
      // These are optional. We also merge them into the corresponding
      // x.* variables.
      //
      // The merging part gets a bit tricky if this module has already
      // been loaded in one of the outer scopes. By doing the straight
      // append we would just be repeating the same options over and
      // over. So what we are going to do is only append to a value if
      // it came from this scope. Then the usage for merging becomes:
      //
      // x.coptions = <overridable options> # Note: '='.
      // using x
      // x.coptions += <overriding options> # Note: '+='.
      //
      rs.assign (x_poptions) += cast_null<strings> (
        config::optional (rs, config_x_poptions));

      rs.assign (x_coptions) += cast_null<strings> (
        config::optional (rs, config_x_coptions));

      rs.assign (x_loptions) += cast_null<strings> (
        config::optional (rs, config_x_loptions));

      rs.assign (x_libs) += cast_null<strings> (
        config::optional (rs, config_x_libs));

      // Load cc.core.config.
      //
      if (!cc_loaded)
      {
        // Prepare configuration hints.
        //
        variable_map h;

        h.assign ("config.cc.id") = cast<string> (rs[x_id]);
        h.assign ("config.cc.target") = cast<string> (rs[x_target]);

        if (!ci.cc_pattern.empty ())
          h.assign ("config.cc.pattern") = move (ci.cc_pattern);

        if (!ci.bin_pattern.empty ())
          h.assign ("config.bin.pattern") = move (ci.bin_pattern);

        load_module ("cc.core.config", rs, rs, loc, false, h);
      }
      else
      {
        // If cc.core.config is already loaded, verify its configuration
        // matched ours since it could have been loaded by another c-family
        // module.
        //
        auto check = [&rs, &loc, this](const char* cvar,
                                      const variable& xvar,
                                      const char* w)
        {
          const string& cv (cast<string> (rs[cvar]));
          const string& xv (cast<string> (rs[xvar]));

          if (cv != xv)
            fail (loc) << "cc and " << x << " module " << w << " mismatch" <<
              info << cvar << " is " << cv <<
              info << xvar.name << " is " << xv;
        };

        // Note that we don't require that patterns match. Presumably, if the
        // toolchain id and target are the same, then where exactly the tools
        // come from doesn't really matter.
        //
        check ("cc.id",     x_id,     "toolchain");
        check ("cc.target", x_target, "target");
      }
    }

    void module::
    init (scope& rs, const location& loc, const variable_map&)
    {
      tracer trace (x, "init");

      // Load cc.core. Besides other things, this will load bin (core) plus
      // extra bin.* modules we may need.
      //
      if (!cast_false<bool> (rs["cc.core.loaded"]))
        load_module ("cc.core", rs, rs, loc);

      // Register target types and configure their "installability".
      //
      {
        using namespace install;

        auto& t (rs.target_types);

        t.insert (x_src);

        // Install headers into install.include.
        //
        for (const target_type* const* ht (x_hdr); *ht != nullptr; ++ht)
        {
          t.insert (**ht);
          install_path (**ht, rs, dir_path ("include"));
        }
      }

      // Register rules.
      //
      {
        using namespace bin;

        auto& r (rs.rules);

        // We register for configure so that we detect unresolved imports
        // during configuration rather that later, e.g., during update.
        //
        // @@ Should we check if install module was loaded (see bin)?
        //
        compile& cr (*this);
        link&    lr (*this);
        install& ir (*this);

        r.insert<obje> (perform_update_id,    x_compile, cr);
        r.insert<obje> (perform_clean_id,     x_compile, cr);
        r.insert<obje> (configure_update_id,  x_compile, cr);

        r.insert<exe>  (perform_update_id,    x_link, lr);
        r.insert<exe>  (perform_clean_id,     x_link, lr);
        r.insert<exe>  (configure_update_id,  x_link, lr);

        r.insert<exe>  (perform_install_id,   x_install, ir);
        r.insert<exe>  (perform_uninstall_id, x_uninstall, ir);

        // Only register static object/library rules if the bin.ar module is
        // loaded (by us or by the user).
        //
        if (cast_false<bool> (rs["bin.ar.loaded"]))
        {
          r.insert<obja> (perform_update_id,    x_compile, cr);
          r.insert<obja> (perform_clean_id,     x_compile, cr);
          r.insert<obja> (configure_update_id,  x_compile, cr);

          r.insert<liba> (perform_update_id,    x_link, lr);
          r.insert<liba> (perform_clean_id,     x_link, lr);
          r.insert<liba> (configure_update_id,  x_link, lr);

          r.insert<liba> (perform_install_id,   x_install, ir);
          r.insert<liba> (perform_uninstall_id, x_uninstall, ir);
        }

        r.insert<objs> (perform_update_id,   x_compile, cr);
        r.insert<objs> (perform_clean_id,    x_compile, cr);
        r.insert<objs> (configure_update_id, x_compile, cr);

        r.insert<libs> (perform_update_id,   x_link, lr);
        r.insert<libs> (perform_clean_id,    x_link, lr);
        r.insert<libs> (configure_update_id, x_link, lr);

        r.insert<libs> (perform_install_id,   x_install, ir);
        r.insert<libs> (perform_uninstall_id, x_uninstall, ir);
      }
    }
  }
}
