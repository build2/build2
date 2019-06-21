// file      : build2/cc/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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

      const variable& config_c_poptions (var_pool["config.cc.poptions"]);
      const variable& config_c_coptions (var_pool["config.cc.coptions"]);
      const variable& config_c_loptions (var_pool["config.cc.loptions"]);

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
        path d;

        if (cc_loaded)
          d = guess_default (x_lang,
                             cast<string> (rs["cc.id"]),
                             cast<string> (rs["cc.pattern"]));
        else
        {
          d = path (x_default);

          if (d.empty ())
            fail << "not built with default " << x_lang << " compiler" <<
              info << "use config." << x << " to specify";
        }

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
      ci_ = &build2::cc::guess (
        x,
        x_lang,
        cast<path> (*p.first),
        cast_null<string> (config::omitted (rs, config_x_id).first),
        cast_null<string> (config::omitted (rs, config_x_version).first),
        cast_null<string> (config::omitted (rs, config_x_target).first),
        cast_null<strings> (rs[config_c_poptions]),
        cast_null<strings> (rs[config_x_poptions]),
        cast_null<strings> (rs[config_c_coptions]),
        cast_null<strings> (rs[config_x_coptions]),
        cast_null<strings> (rs[config_c_loptions]),
        cast_null<strings> (rs[config_x_loptions]));

      const compiler_info& ci (*ci_);

      // Split/canonicalize the target. First see if the user asked us to
      // use config.sub.
      //
      target_triplet tt;
      {
        string ct;

        if (ops.config_sub_specified ())
        {
          ct = run<string> (3,
                            ops.config_sub (),
                            ci.target.c_str (),
                            [] (string& l, bool) {return move (l);});
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

      // Assign values to variables that describe the compiler.
      //
      rs.assign (x_id) = ci.id.string ();
      rs.assign (x_id_type) = to_string (ci.id.type);
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

      if (!x_stdlib.alias (c_stdlib))
        rs.assign (x_stdlib) = ci.x_stdlib;

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

        h.assign (c_runtime) = ci.runtime;
        h.assign (c_stdlib) = ci.c_stdlib;

        load_module (rs, rs, "cc.core.guess", loc, false, h);
      }
      else
      {
        // If cc.core.guess is already loaded, verify its configuration
        // matched ours since it could have been loaded by another c-family
        // module.
        //
        const auto& h (cast<string> (rs["cc.hinter"]));

        auto check = [&loc, &h, this] (const auto& cv,
                                       const auto& xv,
                                       const char* what,
                                       bool error = true)
        {
          if (cv != xv)
          {
            diag_record dr (error ? fail (loc) : warn (loc));

            dr << h << " and " << x << " module " << what << " mismatch" <<
            info << h << " is '" << cv << "'" <<
            info << x << " is '" << xv << "'" <<
            info << "consider explicitly specifying config." << h
                 << " and config." << x;
          }
        };

        check (cast<string> (rs["cc.id"]),
               cast<string> (rs[x_id]),
               "toolchain");

        // We used to not require that patterns match assuming that if the
        // toolchain id and target are the same, then where exactly the tools
        // come from doesn't really matter. But in most cases it will be the
        // g++-7 vs gcc kind of mistakes. So now we warn since even if
        // intentional, it is still probably a bad idea.
        //
        check (cast<string> (rs["cc.pattern"]),
               cast<string> (rs[x_pattern]),
               "toolchain pattern",
               false);

        check (cast<target_triplet> (rs["cc.target"]),
               cast<target_triplet> (rs[x_target]),
               "target");

        check (cast<string> (rs["cc.runtime"]),
               ci.runtime,
               "runtime");

        check (cast<string> (rs["cc.stdlib"]),
               ci.c_stdlib,
               "c standard library");
      }
    }

#ifndef _WIN32
    static const dir_path usr_inc     ("/usr/include");
    static const dir_path usr_loc_lib ("/usr/local/lib");
    static const dir_path usr_loc_inc ("/usr/local/include");
#  ifdef __APPLE__
    static const dir_path a_usr_inc (
      "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include");
#  endif
#endif

    void config_module::
    init (scope& rs, const location& loc, const variable_map&)
    {
      tracer trace (x, "config_init");

      const compiler_info& ci (*ci_);
      const target_triplet& tt (cast<target_triplet> (rs[x_target]));

      // config.x.std overrides x.std
      //
      {
        lookup l (config::omitted (rs, config_x_std).first);

        const string* v;
        if (l.defined ())
        {
          v = cast_null<string> (l);
          rs.assign (x_std) = v;
        }
        else
          v = cast_null<string> (rs[x_std]);

        // Translate x_std value (if any) to the compiler option(s) (if any).
        //
        tstd = translate_std (ci, rs, v);
      }

      // Extract system header/library search paths from the compiler and
      // determine if we need any additional search paths.
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
      // contain /usr[/local]/include then we add /usr/local/*.
      //
      // Note that similar to GCC we also check for the directory existence.
      // Failed that, we can end up with some bizarre yo-yo'ing cases where
      // uninstall removes the directories which in turn triggers a rebuild
      // on the next invocation.
      //
      {
        auto& is (inc_dirs);
        auto& ls (lib_dirs);

        bool ui  (find (is.begin (), is.end (), usr_inc)     != is.end ());
        bool uli (find (is.begin (), is.end (), usr_loc_inc) != is.end ());

#ifdef __APPLE__
        // On Mac OS starting from 10.14 there is no longer /usr/include.
        // Instead we get the following:
        //
        // Homebrew GCC 9:
        //
        // /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include
        //
        // Apple Clang 10.0.1:
        //
        // /Library/Developer/CommandLineTools/usr/include
        // /Library/Developer/CommandLineTools/SDKs/MacOSX10.14.sdk/usr/include
        //
        // What exactly all this means is anyone's guess, of course. So for
        // now we will assume that anything that is or resolves (like that
        // MacOSX10.14.sdk symlink) to:
        //
        // /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include
        //
        // Is Apple's /usr/include.
        //
        if (!ui && !uli)
        {
          for (const dir_path& d: inc_dirs)
          {
            // Both Clang and GCC skip non-existent paths but let's handle
            // (and ignore) directories that cause any errors, for good
            // measure.
            //
            try
            {
              if (d == a_usr_inc || dir_path (d).realize () == a_usr_inc)
              {
                ui = true;
                break;
              }
            }
            catch (...) {}
          }
        }
#endif
        if (ui || uli)
        {
          bool ull (find (ls.begin (), ls.end (), usr_loc_lib) != ls.end ());

          // Many platforms don't search in /usr/local/lib by default (but do
          // for headers in /usr/local/include). So add it as the last option.
          //
          if (!ull && exists (usr_loc_lib, true /* ignore_error */))
            ls.push_back (usr_loc_lib);

          // FreeBSD is at least consistent: it searches in neither. Quoting
          // its wiki: "FreeBSD can't even find libraries that it installed."
          // So let's help it a bit.
          //
          if (!uli && exists (usr_loc_inc, true /* ignore_error */))
            is.push_back (usr_loc_inc);
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
          dr << x << ' ' << project (rs) << '@' << rs << '\n'
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

          if (ct != ci.original_target)
            dr << " (" << ci.original_target << ")";

          dr << "\n  runtime    " << ci.runtime
             << "\n  stdlib     " << ci.x_stdlib;

          if (!x_stdlib.alias (c_stdlib))
            dr << "\n  c stdlib   " << ci.c_stdlib;
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

      rs.assign (x_path) = process_path (ci.path, false /* init */);
      rs.assign (x_sys_lib_dirs) = move (lib_dirs);
      rs.assign (x_sys_inc_dirs) = move (inc_dirs);

      rs.assign (x_signature) = ci.signature;
      rs.assign (x_checksum) = ci.checksum;

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

      rs.assign (x_aoptions) += cast_null<strings> (
        config::optional (rs, config_x_aoptions));

      rs.assign (x_libs) += cast_null<strings> (
        config::optional (rs, config_x_libs));

      // config.x.importable_header
      //
      // It's still fuzzy whether specifying (or maybe tweaking) this list in
      // the configuration will be a common thing to do so for now we use
      // omitted. It's also probably too early to think whether we should have
      // the cc.* version and what the semantics should be.
      //
      if (x_importable_headers != nullptr)
      {
        lookup l (config::omitted (rs, *config_x_importable_headers).first);

        // @@ MODHDR: if(modules) ?
        //
        rs.assign (x_importable_headers) += cast_null<strings> (l);
      }

      // Load cc.core.config.
      //
      if (!cast_false<bool> (rs["cc.core.config.loaded"]))
      {
        variable_map h;

        if (!ci.bin_pattern.empty ())
          h.assign ("config.bin.pattern") = ci.bin_pattern;

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

      // Process, sort, and cache (in this->import_hdr) importable headers.
      // Keep the cache NULL if unused or empty.
      //
      // @@ MODHDR TODO: support exclusions entries (e.g., -<stdio.h>)?
      //
      if (modules && x_importable_headers != nullptr)
      {
        strings* ih (cast_null<strings> (rs.assign (x_importable_headers)));

        if (ih != nullptr && !ih->empty ())
        {
          // Translate <>-style header names to absolute paths using the
          // compiler's include search paths. Otherwise complete and normalize
          // since when searching in this list we always use the absolute and
          // normalized header target path.
          //
          for (string& h: *ih)
          {
            if (h.empty ())
              continue;

            path f;
            if (h.front () == '<' && h.back () == '>')
            {
              h.pop_back ();
              h.erase (0, 1);

              for (const dir_path& d: sys_inc_dirs)
              {
                if (file_exists ((f = d, f /= h),
                                 true /* follow_symlinks */,
                                 true /* ignore_errors */))
                  goto found;
              }

              // What should we do if not found? While we can fail, this could
              // be too drastic if, for example, the header is "optional" and
              // may or may not be present/used. So for now let's restore the
              // original form to aid debugging (it can't possibly match any
              // absolute path).
              //
              h.insert (0, 1, '<');
              h.push_back ('>');
              continue;

            found:
              ; // Fall through.
            }
            else
            {
              f = path (move (h));

              if (f.relative ())
                f.complete ();
            }

            // @@ MODHDR: should we use the more elaborate but robust
            //            normalize/realize scheme so the we get the same
            //            path? Feels right.
            f.normalize ();
            h = move (f).string ();
          }

          sort (ih->begin (), ih->end ());
          import_hdr = ih;
        }
      }

      // Register target types and configure their "installability".
      //
      bool install_loaded (cast_false<bool> (rs["install.loaded"]));

      {
        using namespace install;

        auto& tts (rs.target_types);

        tts.insert (x_src);

        auto insert_hdr = [&rs, &tts, install_loaded] (const target_type& tt)
        {
          tts.insert (tt);

          // Install headers into install.include.
          //
          if (install_loaded)
            install_path (rs, tt, dir_path ("include"));
        };

        // Note: module (x_mod) is in x_hdr.
        //
        for (const target_type* const* ht (x_hdr); *ht != nullptr; ++ht)
          insert_hdr (**ht);

        // Also register the C header for C-derived languages.
        //
        if (*x_hdr != &h::static_type)
          insert_hdr (h::static_type);

        tts.insert<pca> ();
        tts.insert<pcs> ();

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
        const compile_rule& cr (*this);
        const link_rule&    lr (*this);

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

          r.insert<hbmie> (perform_update_id,    x_compile, cr);
          r.insert<hbmie> (perform_clean_id,     x_compile, cr);
          r.insert<hbmie> (configure_update_id,  x_compile, cr);

          r.insert<bmia> (perform_update_id,    x_compile, cr);
          r.insert<bmia> (perform_clean_id,     x_compile, cr);
          r.insert<bmia> (configure_update_id,  x_compile, cr);

          r.insert<hbmia> (perform_update_id,    x_compile, cr);
          r.insert<hbmia> (perform_clean_id,     x_compile, cr);
          r.insert<hbmia> (configure_update_id,  x_compile, cr);

          r.insert<bmis> (perform_update_id,   x_compile, cr);
          r.insert<bmis> (perform_clean_id,    x_compile, cr);
          r.insert<bmis> (configure_update_id, x_compile, cr);

          r.insert<hbmis> (perform_update_id,   x_compile, cr);
          r.insert<hbmis> (perform_clean_id,    x_compile, cr);
          r.insert<hbmis> (configure_update_id, x_compile, cr);
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
          const install_rule&  ir (*this);

          r.insert<exe>  (perform_install_id,   x_install,   ir);
          r.insert<exe>  (perform_uninstall_id, x_uninstall, ir);

          r.insert<liba> (perform_install_id,   x_install,   ir);
          r.insert<liba> (perform_uninstall_id, x_uninstall, ir);

          r.insert<libs> (perform_install_id,   x_install,   ir);
          r.insert<libs> (perform_uninstall_id, x_uninstall, ir);

          const libux_install_rule& lr (*this);

          r.insert<libue> (perform_install_id,   x_install,   lr);
          r.insert<libue> (perform_uninstall_id, x_uninstall, lr);

          r.insert<libua> (perform_install_id,   x_install,   lr);
          r.insert<libua> (perform_uninstall_id, x_uninstall, lr);

          r.insert<libus> (perform_install_id,   x_install,   lr);
          r.insert<libus> (perform_uninstall_id, x_uninstall, lr);
        }
      }
    }
  }
}
