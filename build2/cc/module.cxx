// file      : build2/cc/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/module.hxx>

#include <iomanip> // left, setw()

#include <build2/scope.hxx>
#include <build2/context.hxx>
#include <build2/diagnostics.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/target.hxx> // pc*

#include <build2/config/utility.hxx>
#include <build2/install/utility.hxx>

#include <build2/cc/guess.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    void config_module::
    guess (scope& rs, const location& loc, const variable_map&)
    {
      tracer trace (x, "guess_init");

      bool cc_loaded (cast_false<bool> (rs["cc.core.guess.loaded"]));

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

      if (!p.first)
      {
        // If there is a config.x value for one of the modules that can hint
        // us the toolchain, load it's .guess module. This makes sure that the
        // order in which we load the modules is unimportant and that the user
        // can specify the toolchain using any of the config.x values.
        //
        if (!cc_loaded)
        {
          auto& vp (var_pool.rw (rs));

          for (const char* const* pm (x_hinters); *pm != nullptr; ++pm)
          {
            string m (*pm);

            // Must be the same as in module's init().
            //
            const variable& v (vp.insert<path> ("config." + m, true));

            if (rs[v].defined ())
            {
              load_module (rs, rs, m + ".guess", loc);
              cc_loaded = true;
              break;
            }
          }
        }

        // If cc.core.config is already loaded then use its toolchain id and
        // (optional) pattern to guess an appropriate default (e.g., for {gcc,
        // *-4.9} we will get g++-4.9).
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
        p = config::required (rs,
                              config_x,
                              d,
                              false,
                              cc_loaded ? config::save_commented : 0);
      }

      // Figure out which compiler we are dealing with, its target, etc.
      //
      const path& xc (cast<path> (*p.first));
      ci = build2::cc::guess (x_lang,
                              xc,
                              cast_null<strings> (rs[config_c_coptions]),
                              cast_null<strings> (rs[config_x_coptions]));

      // Split/canonicalize the target. First see if the user asked us to
      // use config.sub.
      //
      target_triplet tt;
      {
        string ct;

        if (ops.config_sub_specified ())
        {
          ct = run<string> (ops.config_sub (),
                            ci.target.c_str (),
                            [] (string& l) {return move (l);});
          l5 ([&]{trace << "config.sub target: '" << ct << "'";});
        }

        try
        {
          tt = target_triplet (ct.empty () ? ci.target : ct);
          l5 ([&]{trace << "canonical target: '" << tt.string () << "'; "
                        << "class: " << tt.class_;});
        }
        catch (const invalid_argument& e)
        {
          // This is where we suggest that the user specifies --config-sub to
          // help us out.
          //
          fail << "unable to parse " << x_lang << " compiler target '"
               << ci.target << "': " << e <<
            info << "consider using the --config-sub option";
        }
      }

      // Assign value to variables that describe the compiler.
      //
      rs.assign (x_id) = ci.id.string ();
      rs.assign (x_id_type) = ci.id.type;
      rs.assign (x_id_variant) = ci.id.variant;

      rs.assign (x_class) = to_string (ci.class_);

      rs.assign (x_version) = ci.version.string;
      rs.assign (x_version_major) = ci.version.major;
      rs.assign (x_version_minor) = ci.version.minor;
      rs.assign (x_version_patch) = ci.version.patch;
      rs.assign (x_version_build) = ci.version.build;

      // Also enter as x.target.{cpu,vendor,system,version,class} for
      // convenience of access.
      //
      rs.assign (x_target_cpu)     = tt.cpu;
      rs.assign (x_target_vendor)  = tt.vendor;
      rs.assign (x_target_system)  = tt.system;
      rs.assign (x_target_version) = tt.version;
      rs.assign (x_target_class)   = tt.class_;

      rs.assign (x_target) = move (tt);

      rs.assign (x_pattern) = ci.pattern;

      new_ = p.second;

      // Load cc.core.guess.
      //
      if (!cc_loaded)
      {
        // Prepare configuration hints.
        //
        variable_map h;

        // Note that all these variables have already been registered.
        //
        h.assign ("config.cc.id") = cast<string> (rs[x_id]);
        h.assign ("config.cc.hinter") = string (x);
        h.assign ("config.cc.target") = cast<target_triplet> (rs[x_target]);

        if (!ci.pattern.empty ())
          h.assign ("config.cc.pattern") = ci.pattern;

        load_module (rs, rs, "cc.core.guess", loc, false, h);
      }
      else
      {
        // If cc.core.guess is already loaded, verify its configuration
        // matched ours since it could have been loaded by another c-family
        // module.
        //
        const auto& h (cast<string> (rs["cc.hinter"]));

        {
          const auto& cv (cast<string> (rs["cc.id"]));
          const auto& xv (cast<string> (rs[x_id]));

          if (cv != xv)
            fail (loc) << h << " and " << x << " module toolchain mismatch" <<
              info << h << " is " << cv <<
              info << x << " is " << xv <<
              info << "consider explicitly specifying config." << h
                       << " and config." << x;
        }

        // We used to not require that patterns match assuming that if the
        // toolchain id and target are the same, then where exactly the tools
        // come from doesn't really matter. But in most cases it will be the
        // g++-7 vs gcc kind of mistakes. So now we warn since even if
        // intentional, it is still a bad idea.
        //
        {
          const auto& cv (cast<string> (rs["cc.pattern"]));
          const auto& xv (cast<string> (rs[x_pattern]));

          if (cv != xv)
            warn (loc) << h << " and " << x << " toolchain pattern mismatch" <<
              info << h << " is '" << cv << "'" <<
              info << x << " is '" << xv << "'" <<
              info << "consider explicitly specifying config." << h
                       << " and config." << x;
        }

        {
          const auto& cv (cast<target_triplet> (rs["cc.target"]));
          const auto& xv (cast<target_triplet> (rs[x_target]));

          if (cv != xv)
            fail (loc) << h << " and " << x << " module target mismatch" <<
              info << h << " target is " << cv <<
              info << x << " target is " << xv;
        }
      }
    }

    void config_module::
    init (scope& rs, const location& loc, const variable_map&)
    {
      tracer trace (x, "config_init");

      const target_triplet& tt (cast<target_triplet> (rs[x_target]));

      // Translate x_std value (if any) to the compiler option(s) (if any).
      //
      tstd = translate_std (ci, rs, cast_null<string> (rs[x_std]));

      // Extract system library search paths from the compiler and determine
      // additional system include search paths.
      //
      dir_paths lib_dirs;
      dir_paths inc_dirs;

      switch (ci.class_)
      {
      case compiler_class::gcc:
        {
          lib_dirs = gcc_library_search_paths (ci.path, rs);
          inc_dirs = gcc_header_search_paths (ci.path, rs);
          break;
        }
      case compiler_class::msvc:
        {
          lib_dirs = msvc_library_search_paths (ci.path, rs);
          inc_dirs = msvc_header_search_paths (ci.path, rs);
          break;
        }
      }

      sys_lib_dirs_extra = lib_dirs.size ();
      sys_inc_dirs_extra = inc_dirs.size ();

#ifndef _WIN32
      // Add /usr/local/{include,lib}. We definitely shouldn't do this if we
      // are cross-compiling. But even if the build and target are the same,
      // it's possible the compiler uses some carefully crafted sysroot and by
      // adding /usr/local/* we will just mess things up. So the heuristics
      // that we will use is this: if the compiler's system include directories
      // contain /usr/include then we add /usr/local/*.
      //
      if (find (inc_dirs.begin (), inc_dirs.end (),
                dir_path ("/usr/include")) != inc_dirs.end ())
      {
        // Many platforms don't search in /usr/local/lib by default (but do
        // for headers in /usr/local/include). So add it as the last option.
        //
        {
          dir_path d ("/usr/local/lib");
          if (find (lib_dirs.begin (), lib_dirs.end (), d) == lib_dirs.end ())
            lib_dirs.emplace_back (move (d));
        }

        // FreeBSD is at least consistent: it searches in neither. Quoting its
        // wiki: "FreeBSD can't even find libraries that it installed." So
        // let's help it a bit.
        //
        {
          dir_path d ("/usr/local/include");
          if (find (inc_dirs.begin (), inc_dirs.end (), d) == inc_dirs.end ())
            inc_dirs.emplace_back (move (d));
        }
      }
#endif

      // If this is a new value (e.g., we are configuring), then print the
      // report at verbosity level 2 and up (-v).
      //
      if (verb >= (new_ ? 2 : 3))
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
          const string& ct (tt.string ()); // Canonical target.

          dr << "  signature  " << ci.signature << '\n'
             << "  checksum   " << ci.checksum << '\n'
             << "  target     " << ct;

          if (ct != ci.target)
            dr << " (" << ci.target << ")";
        }

        if (!tstd.empty ())
        {
          dr << "\n  std       "; // One less space.
          for (const string& o: tstd) dr << ' ' << o;
        }

        if (!ci.pattern.empty ()) // Note: bin_pattern printed by bin
        {
          dr << "\n  pattern    " << ci.pattern;
        }

        if (verb >= 3 && !inc_dirs.empty ())
        {
          dr << "\n  inc dirs";
          for (size_t i (0); i != inc_dirs.size (); ++i)
          {
            if (i == sys_inc_dirs_extra)
              dr << "\n    --";
            dr << "\n    " << inc_dirs[i];
          }
        }

        if (verb >= 3 && !lib_dirs.empty ())
        {
          dr << "\n  lib dirs";
          for (size_t i (0); i != lib_dirs.size (); ++i)
          {
            if (i == sys_lib_dirs_extra)
              dr << "\n    --";
            dr << "\n    " << lib_dirs[i];
          }
        }
      }

      rs.assign (x_path) = move (ci.path);
      rs.assign (x_sys_lib_dirs) = move (lib_dirs);
      rs.assign (x_sys_inc_dirs) = move (inc_dirs);

      rs.assign (x_signature) = move (ci.signature);
      rs.assign (x_checksum) = move (ci.checksum);

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
      if (!cast_false<bool> (rs["cc.core.config.loaded"]))
      {
        variable_map h;

        if (!ci.bin_pattern.empty ())
          h.assign ("config.bin.pattern") = move (ci.bin_pattern);

        load_module (rs, rs, "cc.core.config", loc, false, h);
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
        load_module (rs, rs, "cc.core", loc);

      // Register target types and configure their "installability".
      //
      bool install_loaded (cast_false<bool> (rs["install.loaded"]));

      {
        using namespace install;

        auto& t (rs.target_types);

        t.insert (x_src);

        // Note: module (x_mod) is in x_hdr.

        for (const target_type* const* ht (x_hdr); *ht != nullptr; ++ht)
        {
          t.insert (**ht);

          // Install headers into install.include.
          //
          if (install_loaded)
            install_path (rs, **ht, dir_path ("include"));
        }

        t.insert<pca> ();
        t.insert<pcs> ();

        if (install_loaded)
          install_path<pc> (rs, dir_path ("pkgconfig"));
      }

      // Register rules.
      //
      {
        using namespace bin;

        auto& r (rs.rules);

        // We register for configure so that we detect unresolved imports
        // during configuration rather that later, e.g., during update.
        //
        const compile& cr (*this);
        const link&    lr (*this);

        r.insert<obje> (perform_update_id,    x_compile, cr);
        r.insert<obje> (perform_clean_id,     x_compile, cr);
        r.insert<obje> (configure_update_id,  x_compile, cr);

        r.insert<obja> (perform_update_id,    x_compile, cr);
        r.insert<obja> (perform_clean_id,     x_compile, cr);
        r.insert<obja> (configure_update_id,  x_compile, cr);

        r.insert<objs> (perform_update_id,   x_compile, cr);
        r.insert<objs> (perform_clean_id,    x_compile, cr);
        r.insert<objs> (configure_update_id, x_compile, cr);

        if (modules)
        {
          r.insert<bmie> (perform_update_id,    x_compile, cr);
          r.insert<bmie> (perform_clean_id,     x_compile, cr);
          r.insert<bmie> (configure_update_id,  x_compile, cr);

          r.insert<bmia> (perform_update_id,    x_compile, cr);
          r.insert<bmia> (perform_clean_id,     x_compile, cr);
          r.insert<bmia> (configure_update_id,  x_compile, cr);

          r.insert<bmis> (perform_update_id,   x_compile, cr);
          r.insert<bmis> (perform_clean_id,    x_compile, cr);
          r.insert<bmis> (configure_update_id, x_compile, cr);
        }

        r.insert<libue> (perform_update_id,    x_link, lr);
        r.insert<libue> (perform_clean_id,     x_link, lr);
        r.insert<libue> (configure_update_id,  x_link, lr);

        r.insert<libua> (perform_update_id,    x_link, lr);
        r.insert<libua> (perform_clean_id,     x_link, lr);
        r.insert<libua> (configure_update_id,  x_link, lr);

        r.insert<libus> (perform_update_id,    x_link, lr);
        r.insert<libus> (perform_clean_id,     x_link, lr);
        r.insert<libus> (configure_update_id,  x_link, lr);

        r.insert<exe>  (perform_update_id,    x_link, lr);
        r.insert<exe>  (perform_clean_id,     x_link, lr);
        r.insert<exe>  (configure_update_id,  x_link, lr);

        r.insert<liba> (perform_update_id,    x_link, lr);
        r.insert<liba> (perform_clean_id,     x_link, lr);
        r.insert<liba> (configure_update_id,  x_link, lr);

        r.insert<libs> (perform_update_id,   x_link, lr);
        r.insert<libs> (perform_clean_id,    x_link, lr);
        r.insert<libs> (configure_update_id, x_link, lr);

        // Note that while libu*{} are not installable, we need to see through
        // them in case they depend on stuff that we need to install (see the
        // install rule implementations for details).
        //
        if (install_loaded)
        {
          const file_install&  fr (*this);
          const alias_install& ar (*this);

          r.insert<exe>  (perform_install_id,   x_install,   fr);
          r.insert<exe>  (perform_uninstall_id, x_uninstall, fr);

          r.insert<liba> (perform_install_id,   x_install,   fr);
          r.insert<liba> (perform_uninstall_id, x_uninstall, fr);

          r.insert<libs> (perform_install_id,   x_install,   fr);
          r.insert<libs> (perform_uninstall_id, x_uninstall, fr);

          r.insert<libue> (perform_install_id,   x_install,   ar);
          r.insert<libue> (perform_uninstall_id, x_uninstall, ar);

          r.insert<libua> (perform_install_id,   x_install,   ar);
          r.insert<libua> (perform_uninstall_id, x_uninstall, ar);

          r.insert<libus> (perform_install_id,   x_install,   ar);
          r.insert<libus> (perform_uninstall_id, x_uninstall, ar);
        }
      }
    }
  }
}
