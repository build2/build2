// file      : libbuild2/cc/module.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/module.hxx>

#include <iomanip> // left, setw()

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/cc/guess.hxx>

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

      context& ctx (rs.ctx);

      bool cc_loaded (cast_false<bool> (rs["cc.core.guess.loaded"]));

      // Adjust module priority (compiler). Also order cc module before us
      // (we don't want to use priorities for that in case someone manages
      // to slot in-between).
      //
      if (!cc_loaded)
        config::save_module (rs, "cc", 250);

      config::save_module (rs, x, 250);

      // All the variables we enter are qualified so go straight for the
      // public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));

      // Must already exist.
      //
      const variable& config_c_poptions (vp["config.cc.poptions"]);
      const variable& config_c_coptions (vp["config.cc.coptions"]);
      const variable& config_c_loptions (vp["config.cc.loptions"]);

      // Configuration.
      //
      using config::lookup_config;

      // config.x
      //
      strings omode; // Original mode.
      {
        // Normally we will have a persistent configuration and computing the
        // default value every time will be a waste. So try without a default
        // first.
        //
        lookup l (lookup_config (new_config, rs, config_x));

        if (!l)
        {
          // If there is a config.x value for one of the modules that can hint
          // us the toolchain, load it's .guess module. This makes sure that
          // the order in which we load the modules is unimportant and that
          // the user can specify the toolchain using any of the config.x
          // values.
          //
          if (!cc_loaded)
          {
            for (const char* const* pm (x_hinters); *pm != nullptr; ++pm)
            {
              string m (*pm);

              // Must be the same as in module's init().
              //
              const variable& v (vp.insert<strings> ("config." + m));

              if (rs[v].defined ())
              {
                init_module (rs, rs, m + ".guess", loc);
                cc_loaded = true;
                break;
              }
            }
          }

          // If cc.core.guess is already loaded then use its toolchain id,
          // (optional) pattern, and mode to guess an appropriate default
          // (e.g., for {gcc, *-4.9 -m64} we will get g++-4.9 -m64).
          //
          strings d;

          if (cc_loaded)
            d = guess_default (x_lang,
                               cast<string>  (rs["cc.id"]),
                               cast<string>  (rs["cc.pattern"]),
                               cast<strings> (rs["cc.mode"]));
          else
          {
            // Note that we don't have the default mode: it doesn't feel
            // correct to default to, say, -m64 simply because that's how
            // build2 was built.
            //
            d.push_back (x_default);

            if (d.front ().empty ())
              fail << "not built with default " << x_lang << " compiler" <<
                info << "use " << config_x << " to specify";
          }

          // If this value was hinted, save it as commented out so that if the
          // user changes the source of the pattern/mode, this one will get
          // updated as well.
          //
          l = lookup_config (new_config,
                             rs,
                             config_x,
                             move (d),
                             cc_loaded ? config::save_default_commented : 0);
        }

        // Split the value into the compiler path and mode.
        //
        const strings& v (cast<strings> (l));

        path xc;
        {
          const string& s (v.empty () ? empty_string : v.front ());

          try { xc = path (s); } catch (const invalid_path&) {}

          if (xc.empty ())
            fail << "invalid path '" << s << "' in " << config_x;
        }

        omode.assign (++v.begin (), v.end ());

        // Save original path/mode in *.config.path/mode.
        //
        rs.assign (x_c_path) = xc;
        rs.assign (x_c_mode) = omode;

        // Merge the configured mode options into user-specified (which must
        // be done before loading the *.guess module).
        //
        // In particular, this ability to specify the compiler mode in a
        // buildfile is useful in embedded development where the project may
        // need to hardcode things like -target, -nostdinc, etc.
        //
        const strings& mode (cast<strings> (rs.assign (x_mode) += omode));

        // Figure out which compiler we are dealing with, its target, etc.
        //
        // Note that we could allow guess() to modify mode to support
        // imaginary options (such as /MACHINE for cl.exe). Though it's not
        // clear what cc.mode would contain (original or modified). Note that
        // we are now adding *.std options into mode options.
        //
        // @@ But can't the language standard options alter things like search
        //    directories?
        //
        x_info = &build2::cc::guess (
          ctx,
          x, x_lang,
          rs.root_extra->environment_checksum,
          move (xc),
          cast_null<string> (lookup_config (rs, config_x_id)),
          cast_null<string> (lookup_config (rs, config_x_version)),
          cast_null<string> (lookup_config (rs, config_x_target)),
          mode,
          cast_null<strings> (rs[config_c_poptions]),
          cast_null<strings> (rs[config_x_poptions]),
          cast_null<strings> (rs[config_c_coptions]),
          cast_null<strings> (rs[config_x_coptions]),
          cast_null<strings> (rs[config_c_loptions]),
          cast_null<strings> (rs[config_x_loptions]));
      }

      const compiler_info& xi (*x_info);

      // Split/canonicalize the target. First see if the user asked us to use
      // config.sub.
      //
      target_triplet tt;
      {
        string ct;

        if (config_sub)
        {
          ct = run<string> (ctx,
                            3,
                            *config_sub,
                            xi.target.c_str (),
                            [] (string& l, bool) {return move (l);});
          l5 ([&]{trace << "config.sub target: '" << ct << "'";});
        }

        try
        {
          tt = target_triplet (ct.empty () ? xi.target : ct);
          l5 ([&]{trace << "canonical target: '" << tt.string () << "'; "
                        << "class: " << tt.class_;});
        }
        catch (const invalid_argument& e)
        {
          // This is where we suggest that the user specifies --config-sub to
          // help us out.
          //
          fail << "unable to parse " << x_lang << " compiler target '"
               << xi.target << "': " << e <<
            info << "consider using the --config-sub option";
        }
      }

      // Hash the environment (used for change detection).
      //
      // Note that for simplicity we use the combined checksum for both
      // compilation and linking (which may compile, think LTO).
      //
      {
        sha256 cs;
        hash_environment (cs, xi.compiler_environment);
        hash_environment (cs, xi.platform_environment);
        env_checksum = cs.string ();
      }

      // Assign values to variables that describe the compiler.
      //
      // Note: x_mode is dealt with above.
      //
      rs.assign (x_path) = process_path_ex (
        xi.path, x_name, xi.checksum, env_checksum);

      rs.assign (x_id) = xi.id.string ();
      rs.assign (x_id_type) = to_string (xi.id.type);
      rs.assign (x_id_variant) = xi.id.variant;

      rs.assign (x_class) = to_string (xi.class_);

      auto assign_version = [&rs] (const variable** vars,
                                   const compiler_version* v)
      {
        rs.assign (vars[0]) = v != nullptr ? value (v->string) : value ();
        rs.assign (vars[1]) = v != nullptr ? value (v->major) : value ();
        rs.assign (vars[2]) = v != nullptr ? value (v->minor) : value ();
        rs.assign (vars[3]) = v != nullptr ? value (v->patch) : value ();
        rs.assign (vars[4]) = v != nullptr ? value (v->build) : value ();
      };

      assign_version (&x_version, &xi.version);
      assign_version (&x_variant_version,
                      xi.variant_version ? &*xi.variant_version : nullptr);

      rs.assign (x_signature) = xi.signature;
      rs.assign (x_checksum) = xi.checksum;

      // Also enter as x.target.{cpu,vendor,system,version,class} for
      // convenience of access.
      //
      rs.assign (x_target_cpu)     = tt.cpu;
      rs.assign (x_target_vendor)  = tt.vendor;
      rs.assign (x_target_system)  = tt.system;
      rs.assign (x_target_version) = tt.version;
      rs.assign (x_target_class)   = tt.class_;

      rs.assign (x_target) = move (tt);

      rs.assign (x_pattern) = xi.pattern;

      if (!x_stdlib.alias (c_stdlib))
        rs.assign (x_stdlib) = xi.x_stdlib;

      // Load cc.core.guess.
      //
      if (!cc_loaded)
      {
        // Prepare configuration hints (pretend it belongs to root scope).
        //
        variable_map h (rs);

        // Note that all these variables have already been registered.
        //
        h.assign ("config.cc.id") = cast<string> (rs[x_id]);
        h.assign ("config.cc.hinter") = string (x);
        h.assign ("config.cc.target") = cast<target_triplet> (rs[x_target]);

        if (!xi.pattern.empty ())
          h.assign ("config.cc.pattern") = xi.pattern;

        if (!omode.empty ())
          h.assign ("config.cc.mode") = move (omode);

        h.assign (c_runtime) = xi.runtime;
        h.assign (c_stdlib) = xi.c_stdlib;

        init_module (rs, rs, "cc.core.guess", loc, false, h);
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
        // Note also that it feels right to allow different modes (think
        // -fexceptions for C or -fno-rtti for C++).
        //
        check (cast<string> (rs["cc.pattern"]),
               cast<string> (rs[x_pattern]),
               "toolchain pattern",
               false);

        check (cast<target_triplet> (rs["cc.target"]),
               cast<target_triplet> (rs[x_target]),
               "target");

        check (cast<string> (rs["cc.runtime"]),
               xi.runtime,
               "runtime");

        check (cast<string> (rs["cc.stdlib"]),
               xi.c_stdlib,
               "c standard library");
      }
    }

#ifndef _WIN32
    static const dir_path usr_inc     ("/usr/include");
    static const dir_path usr_loc_lib ("/usr/local/lib");
    static const dir_path usr_loc_inc ("/usr/local/include");
#  ifdef __APPLE__
    static const dir_path a_usr_inc (
      "/Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk/usr/include");
    static const dir_path a_usr_lib (
      "/Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk/usr/lib");
#  endif
#endif

    // Extracting search dirs can be expensive (we may need to run the
    // compiler several times) so we cache the result.
    //
    struct search_dirs
    {
      pair<dir_paths, size_t> lib;
      pair<dir_paths, size_t> hdr;
    };

    static global_cache<search_dirs> dirs_cache;

    void config_module::
    init (scope& rs, const location& loc, const variable_map&)
    {
      tracer trace (x, "config_init");

      const compiler_info& xi (*x_info);
      const target_triplet& tt (cast<target_triplet> (rs[x_target]));

      // Load cc.core.config.
      //
      if (!cast_false<bool> (rs["cc.core.config.loaded"]))
      {
        // Prepare configuration hints (pretend it belongs to root scope).
        //
        variable_map h (rs);

        if (!xi.bin_pattern.empty ())
          h.assign ("config.bin.pattern") = xi.bin_pattern;

        init_module (rs, rs, "cc.core.config", loc, false, h);
      }

      // Configuration.
      //
      using config::lookup_config;

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
      // @@ There are actually two cases to this issue:
      //
      //    1. The module is loaded in the outer project (e.g., tests inside a
      //       project). It feels like this should be handled with project
      //       variable visibility. And now it is with the project being the
      //       default. Note that this is the reason we don't need any of this
      //       for the project configuration: there the config.* variables are
      //       always set on the project root.
      //
      //    2. The module is loaded in the outer scope within the same
      //       project. We are currently thinking whether we should even
      //       support loading of modules in non-root scopes.
      //
      // x.coptions = <overridable options> # Note: '='.
      // using x
      // x.coptions += <overriding options> # Note: '+='.
      //
      rs.assign (x_poptions) += cast_null<strings> (
        lookup_config (rs, config_x_poptions, nullptr));

      rs.assign (x_coptions) += cast_null<strings> (
        lookup_config (rs, config_x_coptions, nullptr));

      rs.assign (x_loptions) += cast_null<strings> (
        lookup_config (rs, config_x_loptions, nullptr));

      rs.assign (x_aoptions) += cast_null<strings> (
        lookup_config (rs, config_x_aoptions, nullptr));

      rs.assign (x_libs) += cast_null<strings> (
        lookup_config (rs, config_x_libs, nullptr));

      // config.x.std overrides x.std
      //
      strings& mode (cast<strings> (rs.assign (x_mode))); // Set by guess.
      {
        lookup l (lookup_config (rs, config_x_std));

        const string* v;
        if (l.defined ())
        {
          v = cast_null<string> (l);
          rs.assign (x_std) = v;
        }
        else
          v = cast_null<string> (rs[x_std]);

        // Translate x_std value (if any) to the compiler option(s) (if any)
        // and fold them into the compiler mode.
        //
        translate_std (xi, tt, rs, mode, v);
      }

      // config.x.internal.scope
      //
      // Note: save omitted.
      //
      // The effective internal_scope value is chosen based on the following
      // priority list:
      //
      // 1. config.x.internal.scope
      //
      // 2. config.cc.internal.scope
      //
      // 3. effective value from bundle amalgamation
      //
      // 4. x.internal.scope
      //
      // 5. cc.internal.scope
      //
      // Note also that we only update x.internal.scope (and not cc.*) to
      // reflect the effective value.
      //
      const string* iscope_str (nullptr);
      {
        if (lookup l = lookup_config (rs, config_x_internal_scope))       // 1
        {
          iscope_str = &cast<string> (l);

          if (*iscope_str == "current")
            fail << "'current' value in " << config_x_internal_scope;
        }
        else if (lookup l = rs["config.cc.internal.scope"])               // 2
        {
          iscope_str = &cast<string> (l);
        }
        else                                                              // 3
        {
          const scope& as (*rs.bundle_scope ());

          if (as != rs)
          {
            // Only use the value if the corresponding module is loaded.
            //
            bool xl (cast_false<bool> (as[string (x) + ".config.loaded"]));
            if (xl)
              iscope_str = cast_null<string> (as[x_internal_scope]);

            if (iscope_str == nullptr)
            {
              if (xl || cast_false<bool> (as["cc.core.config.loaded"]))
                iscope_str = cast_null<string> (as["cc.internal.scope"]);
            }

            if (iscope_str != nullptr && *iscope_str == "current")
              iscope_current = &as;
          }
        }

        lookup l;
        if (iscope_str == nullptr)
        {
          iscope_str = cast_null<string> (l = rs[x_internal_scope]);      // 4

          if (iscope_str == nullptr)
            iscope_str = cast_null<string> (rs["cc.internal.scope"]);     // 5
        }

        if (iscope_str != nullptr)
        {
          const string& s (*iscope_str);

          // Assign effective.
          //
          if (!l)
            rs.assign (x_internal_scope) = s;

          if (s == "current")
          {
            iscope = internal_scope::current;

            if (iscope_current == nullptr)
              iscope_current = &rs;
          }
          else if (s == "base")   iscope = internal_scope::base;
          else if (s == "root")   iscope = internal_scope::root;
          else if (s == "bundle") iscope = internal_scope::bundle;
          else if (s == "strong") iscope = internal_scope::strong;
          else if (s == "weak")   iscope = internal_scope::weak;
          else if (s == "global")
            ; // Nothing to translate;
          else
            fail << "invalid " << x_internal_scope << " value '" << s << "'";
        }
      }

      // config.x.translate_include
      //
      // It's still fuzzy whether specifying (or maybe tweaking) this list in
      // the configuration will be a common thing to do so for now we use
      // omitted.
      //
      if (x_translate_include != nullptr)
      {
        if (lookup l = lookup_config (rs, *config_x_translate_include))
        {
          // @@ MODHDR: if(modules) ? Yes.
          //
          rs.assign (x_translate_include).prepend (
            cast<translatable_headers> (l));
        }
      }

      // Extract system header/library search paths from the compiler and
      // determine if we need any additional search paths.
      //
      // Note that for now module search paths only come from compiler_info.
      //
      pair<dir_paths, size_t> lib_dirs;
      pair<dir_paths, size_t> hdr_dirs;
      const optional<pair<dir_paths, size_t>>& mod_dirs (xi.sys_mod_dirs);

      if (xi.sys_lib_dirs && xi.sys_hdr_dirs)
      {
        lib_dirs = *xi.sys_lib_dirs;
        hdr_dirs = *xi.sys_hdr_dirs;
      }
      else
      {
        string key;
        {
          sha256 cs;
          cs.append (static_cast<size_t> (x_lang));
          cs.append (xi.path.effect_string ());
          append_options (cs, mode);
          key = cs.string ();
        }

        // Because the compiler info (xi) is also cached, we can assume that
        // if dirs come from there, then they do so consistently.
        //
        const search_dirs* sd (dirs_cache.find (key));

        if (xi.sys_lib_dirs)
          lib_dirs = *xi.sys_lib_dirs;
        else if (sd != nullptr)
          lib_dirs = sd->lib;
        else
        {
          switch (xi.class_)
          {
          case compiler_class::gcc:
            lib_dirs = gcc_library_search_dirs (xi, rs);
            break;
          case compiler_class::msvc:
            lib_dirs = msvc_library_search_dirs (xi, rs);
            break;
          }
        }

        if (xi.sys_hdr_dirs)
          hdr_dirs = *xi.sys_hdr_dirs;
        else if (sd != nullptr)
          hdr_dirs = sd->hdr;
        else
        {
          switch (xi.class_)
          {
          case compiler_class::gcc:
            hdr_dirs = gcc_header_search_dirs (xi, rs);
            break;
          case compiler_class::msvc:
            hdr_dirs = msvc_header_search_dirs (xi, rs);
            break;
          }
        }

        if (sd == nullptr)
        {
          search_dirs sd;
          if (!xi.sys_lib_dirs) sd.lib = lib_dirs;
          if (!xi.sys_hdr_dirs) sd.hdr = hdr_dirs;
          dirs_cache.insert (move (key), move (sd));
        }
      }

      sys_lib_dirs_mode = lib_dirs.second;
      sys_hdr_dirs_mode = hdr_dirs.second;
      sys_mod_dirs_mode = mod_dirs ? mod_dirs->second : 0;

      sys_lib_dirs_extra = 0;
      sys_hdr_dirs_extra = 0;

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
        auto& hs (hdr_dirs.first);
        auto& ls (lib_dirs.first);

        bool ui  (find (hs.begin (), hs.end (), usr_inc)     != hs.end ());
        bool uli (find (hs.begin (), hs.end (), usr_loc_inc) != hs.end ());

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
        // What exactly all this means is anyone's guess, of course (see
        // homebrew-core issue #46393 for some background). So for now we
        // will assume that anything that matches this pattern:
        //
        // /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk/usr/include
        //
        // Is Apple's /usr/include.
        //
        // Also, it appears neither Clang nor GCC report MacOSX*.sdk/usr/lib
        // with -print-search-dirs but they do search in there. So we add it
        // to our list if we see MacOSX*.sdk/usr/include.
        //
        auto aui (find_if (hs.begin (), hs.end (),
                           [] (const dir_path& d)
                           {
                             return path_match (d, a_usr_inc);
                           }));

        if (aui != hs.end ())
        {
          if (!ui)
            ui = true;

          if (find_if (ls.begin (), ls.end (),
                       [] (const dir_path& d)
                       {
                         return path_match (d, a_usr_lib);
                       }) == ls.end ())
          {
            ls.push_back (aui->directory () /= "lib");
          }
        }
#endif
        if (ui || uli)
        {
          bool ull (find (ls.begin (), ls.end (), usr_loc_lib) != ls.end ());

          // Many platforms don't search in /usr/local/lib by default but do
          // for headers in /usr/local/include.
          //
          // Note that customarily /usr/local/include is searched before
          // /usr/include so we add /usr/local/lib before built-in entries
          // (there isn't really a way to add it after since all we can do is
          // specify it with -L).
          //
          if (!ull && exists (usr_loc_lib, true /* ignore_error */))
          {
            ls.insert (ls.begin () + sys_lib_dirs_mode, usr_loc_lib);
            ++sys_lib_dirs_extra;
          }

          // FreeBSD is at least consistent: it searches in neither. Quoting
          // its wiki: "FreeBSD can't even find libraries that it installed."
          // So let's help it a bit.
          //
          if (!uli && exists (usr_loc_inc, true /* ignore_error */))
          {
            hs.insert (hs.begin () + sys_hdr_dirs_mode, usr_loc_inc);
            ++sys_hdr_dirs_extra;
          }
        }
      }
#endif

      // If this is a configuration with new values, then print the report
      // at verbosity level 2 and up (-v).
      //
      if (verb >= (new_config ? 2 : 3))
      {
        diag_record dr (text);

        {
          dr << x << ' ' << project (rs) << '@' << rs << '\n'
             << "  " << left << setw (11) << x << xi.path << '\n';
        }

        if (!mode.empty ())
        {
          dr << "  mode      "; // One space short.

          for (const string& o: mode)
            dr << ' ' << o;

          dr << '\n';
        }

        {
          dr << "  id         " << xi.id << '\n'
             << "  version    " << xi.version.string << '\n'
             << "  major      " << xi.version.major << '\n'
             << "  minor      " << xi.version.minor << '\n'
             << "  patch      " << xi.version.patch << '\n';
        }

        if (!xi.version.build.empty ())
        {
          dr << "  build      " << xi.version.build << '\n';
        }

        if (xi.variant_version)
        {
          dr << "  variant:" << '\n'
             << "    version  " << xi.variant_version->string << '\n'
             << "    major    " << xi.variant_version->major << '\n'
             << "    minor    " << xi.variant_version->minor << '\n'
             << "    patch    " << xi.variant_version->patch << '\n';
        }

        if (xi.variant_version && !xi.variant_version->build.empty ())
        {
          dr << "    build    " << xi.variant_version->build << '\n';
        }

        {
          const string& ct (tt.string ()); // Canonical target.

          dr << "  signature  " << xi.signature << '\n'
             << "  checksum   " << xi.checksum << '\n'
             << "  target     " << ct;

          if (ct != xi.original_target)
            dr << " (" << xi.original_target << ")";

          dr << "\n  runtime    " << xi.runtime
             << "\n  stdlib     " << xi.x_stdlib;

          if (!x_stdlib.alias (c_stdlib))
            dr << "\n  c stdlib   " << xi.c_stdlib;
        }

        if (!xi.pattern.empty ()) // Note: bin_pattern printed by bin
        {
          dr << "\n  pattern    " << xi.pattern;
        }

        auto& mods (mod_dirs ? mod_dirs->first : dir_paths ());
        auto& incs (hdr_dirs.first);
        auto& libs (lib_dirs.first);

        if (verb >= 3 && iscope)
        {
          dr << "\n  int scope  ";

          if (*iscope == internal_scope::current)
            dr << iscope_current->out_path ();
          else
            dr << *iscope_str;
        }

        if (verb >= 3 && !mods.empty ())
        {
          dr << "\n  mod dirs";
          for (const dir_path& d: mods)
          {
            dr << "\n    " << d;
          }
        }

        if (verb >= 3 && !incs.empty ())
        {
          dr << "\n  hdr dirs";
          for (size_t i (0); i != incs.size (); ++i)
          {
            if ((sys_hdr_dirs_mode  != 0 && i == sys_hdr_dirs_mode) ||
                (sys_hdr_dirs_extra != 0 &&
                 i == sys_hdr_dirs_extra + sys_hdr_dirs_mode))
              dr << "\n    --";

            dr << "\n    " << incs[i];
          }
        }

        if (verb >= 3 && !libs.empty ())
        {
          dr << "\n  lib dirs";
          for (size_t i (0); i != libs.size (); ++i)
          {
            if ((sys_lib_dirs_mode  != 0 && i == sys_lib_dirs_mode) ||
                (sys_lib_dirs_extra != 0 &&
                 i == sys_lib_dirs_extra + sys_lib_dirs_mode))
              dr << "\n    --";

            dr << "\n    " << libs[i];
          }
        }
      }

      rs.assign (x_sys_lib_dirs) = move (lib_dirs.first);
      rs.assign (x_sys_hdr_dirs) = move (hdr_dirs.first);

      config::save_environment (rs, xi.compiler_environment);
      config::save_environment (rs, xi.platform_environment);
    }

    // Global cache of ad hoc importable headers.
    //
    // The key is a hash of the system header search directories
    // (sys_hdr_dirs) where we search for the headers.
    //
    static map<string, importable_headers> importable_headers_cache;
    static mutex importable_headers_mutex;

    void module::
    init (scope& rs,
          const location& loc,
          const variable_map&,
          const compiler_info& xi)
    {
      tracer trace (x, "init");

      context& ctx (rs.ctx);

      // Register the module function family if this is the first instance of
      // this modules.
      //
      if (!function_family::defined (ctx.functions, x))
      {
        function_family f (ctx.functions, x);
        compile_rule::functions (f, x);
        link_rule::functions (f, x);
      }

      // Load cc.core. Besides other things, this will load bin (core) plus
      // extra bin.* modules we may need.
      //
      load_module (rs, rs, "cc.core", loc);

      // Search include translation headers and groups.
      //
      if (modules)
      {
        {
          sha256 k;
          for (const dir_path& d: sys_hdr_dirs)
            k.append (d.string ());

          mlock l (importable_headers_mutex);
          importable_headers = &importable_headers_cache[k.string ()];
        }

        auto& hs (*importable_headers);

        ulock ul (hs.mutex);

        if (hs.group_map.find (header_group_std) == hs.group_map.end ())
          guess_std_importable_headers (xi, sys_hdr_dirs, hs);

        // Process x.translate_include.
        //
        const variable& var (*x_translate_include);
        if (auto* v = cast_null<translatable_headers> (rs[var]))
        {
          for (const auto& p: *v)
          {
            const string& k (p.first);

            if (k.front () == '<' && k.back () == '>')
            {
              if (path_pattern (k))
              {
                size_t n (hs.insert_angle_pattern (sys_hdr_dirs, k));

                l5 ([&]{trace << "pattern " << k << " searched to " << n
                              << " headers";});
              }
              else
              {
                // What should we do if not found? While we can fail, this
                // could be too drastic if, for example, the header is
                // "optional" and may or may not be present/used. So for now
                // let's ignore (we could have also removed it from the map as
                // an indication).
                //
                const auto* r (hs.insert_angle (sys_hdr_dirs, k));

                l5 ([&]{trace << "header " << k << " searched to "
                              << (r ? r->first.string ().c_str () : "<none>");});
              }
            }
            else if (path_traits::find_separator (k) == string::npos)
            {
              // Group name.
              //
              if (k != header_group_all_importable &&
                  k != header_group_std_importable &&
                  k != header_group_all            &&
                  k != header_group_std)
                fail (loc) << "unknown header group '" << k << "' in " << var;
            }
            else
            {
              // Absolute and normalized header path.
              //
              if (!path_traits::absolute (k))
                fail (loc) << "relative header path '" << k << "' in " << var;
            }
          }
        }
      }

      // Register target types and configure their "installability".
      //
      load_module (rs, rs, (string (x) += ".types"), loc);

      // Register rules.
      //
      {
        using namespace bin;

        // If the target doesn't support shared libraries, then don't register
        // the corresponding rules.
        //
        bool s (tsys != "emscripten");

        auto& r (rs.rules);
        const compile_rule& cr (*this);
        const link_rule&    lr (*this);

        // We register for configure so that we detect unresolved imports
        // during configuration rather that later, e.g., during update.
        //
        r.insert<obje> (perform_update_id,    x_compile, cr);
        r.insert<obje> (perform_clean_id,     x_compile, cr);
        r.insert<obje> (configure_update_id,  x_compile, cr);

        r.insert<obja> (perform_update_id,    x_compile, cr);
        r.insert<obja> (perform_clean_id,     x_compile, cr);
        r.insert<obja> (configure_update_id,  x_compile, cr);

        if (s)
        {
          r.insert<objs> (perform_update_id,   x_compile, cr);
          r.insert<objs> (perform_clean_id,    x_compile, cr);
          r.insert<objs> (configure_update_id, x_compile, cr);
        }

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

          if (s)
          {
            r.insert<bmis> (perform_update_id,   x_compile, cr);
            r.insert<bmis> (perform_clean_id,    x_compile, cr);
            r.insert<bmis> (configure_update_id, x_compile, cr);

            r.insert<hbmis> (perform_update_id,   x_compile, cr);
            r.insert<hbmis> (perform_clean_id,    x_compile, cr);
            r.insert<hbmis> (configure_update_id, x_compile, cr);
          }
        }

        r.insert<libue> (perform_update_id,    x_link, lr);
        r.insert<libue> (perform_clean_id,     x_link, lr);
        r.insert<libue> (configure_update_id,  x_link, lr);

        r.insert<libua> (perform_update_id,    x_link, lr);
        r.insert<libua> (perform_clean_id,     x_link, lr);
        r.insert<libua> (configure_update_id,  x_link, lr);

        if (s)
        {
          r.insert<libus> (perform_update_id,    x_link, lr);
          r.insert<libus> (perform_clean_id,     x_link, lr);
          r.insert<libus> (configure_update_id,  x_link, lr);
        }

        r.insert<exe>  (perform_update_id,    x_link, lr);
        r.insert<exe>  (perform_clean_id,     x_link, lr);
        r.insert<exe>  (configure_update_id,  x_link, lr);

        r.insert<liba> (perform_update_id,    x_link, lr);
        r.insert<liba> (perform_clean_id,     x_link, lr);
        r.insert<liba> (configure_update_id,  x_link, lr);

        if (s)
        {
          r.insert<libs> (perform_update_id,   x_link, lr);
          r.insert<libs> (perform_clean_id,    x_link, lr);
          r.insert<libs> (configure_update_id, x_link, lr);
        }

        // Note that while libu*{} are not installable, we need to see through
        // them in case they depend on stuff that we need to install (see the
        // install rule implementations for details).
        //
        if (cast_false<bool> (rs["install.loaded"]))
        {
          // Note: we rely quite heavily in these rule implementations that
          // these are the only target types they are registered for.

          const install_rule&  ir (*this);

          r.insert<exe>  (perform_install_id,   x_install, ir);
          r.insert<exe>  (perform_uninstall_id, x_install, ir);

          r.insert<liba> (perform_install_id,   x_install, ir);
          r.insert<liba> (perform_uninstall_id, x_install, ir);

          if (s)
          {
            r.insert<libs> (perform_install_id,   x_install, ir);
            r.insert<libs> (perform_uninstall_id, x_install, ir);
          }

          const libux_install_rule& lr (*this);

          r.insert<libue> (perform_install_id,   x_install, lr);
          r.insert<libue> (perform_uninstall_id, x_install, lr);

          r.insert<libua> (perform_install_id,   x_install, lr);
          r.insert<libua> (perform_uninstall_id, x_install, lr);

          if (s)
          {
            r.insert<libus> (perform_install_id,   x_install, lr);
            r.insert<libus> (perform_uninstall_id, x_install, lr);
          }
        }
      }
    }
  }
}
