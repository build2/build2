// file      : libbuild2/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/module.hxx>

#ifndef _WIN32
#  include <dlfcn.h>
#else
#  include <libbutl/win32-utility.hxx>
#endif

#include <libbuild2/file.hxx>  // import()
#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

// Core modules bundled with libbuild2.
//
#include <libbuild2/dist/init.hxx>
#include <libbuild2/test/init.hxx>
#include <libbuild2/config/init.hxx>
#include <libbuild2/install/init.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  loaded_module_map loaded_modules;

  // Sorted array of bundled modules (excluding core modules bundled with
  // libbuild2; see below).
  //
  static const char* bundled_modules[] = {
    "bash",
    "in",
    "version"
  };

  static inline bool
  bundled_module (const string& mod)
  {
    return binary_search (
      bundled_modules,
      bundled_modules + sizeof (bundled_modules) / sizeof (*bundled_modules),
      mod);
  }

  static module_load_function*
  import_module (scope& /*bs*/,
                 const string& mod,
                 const location& loc,
                 bool boot,
                 bool opt)
  {
    // Take care of core modules that are bundled with libbuild2 in case they
    // are not pre-loaded by the driver.
    //
    if      (mod == "config")  return &config::build2_config_load;
    else if (mod == "dist")    return &dist::build2_dist_load;
    else if (mod == "install") return &install::build2_install_load;
    else if (mod == "test")    return &test::build2_test_load;

    bool bundled (bundled_module (mod));

    // Importing external modules during bootstrap is problematic: we haven't
    // loaded config.build nor entered all the variable overrides so it's not
    // clear what import() can do except confuse matters. So this requires
    // more thinking.
    //
    if (boot && !bundled)
    {
      fail (loc) << "unable to load build system module " << mod <<
        info << "loading external modules during bootstrap is not yet "
                 << "supported";
    }

    path lib;

#if 0
    // See if we can import a target for this module.
    //
    // Check if one of the bundled modules, if so, the project name is
    // build2, otherwise -- libbuild2-<mod>.
    //
    // The target we are looking for is <prj>%lib{build2-<mod>}.
    //
    name tgt (
      import (bs,
              name (bundled ? "build2" : "libbuild2-" + mod,
                    dir_path (),
                    "lib",
                    "build2-" + mod),
              loc));

    if (!tgt.qualified ())
    {
      // Switch the phase and update the target. This will also give us the
      // shared library path.
      //
      // @@ TODO
      //
    }
    else
#endif
    {
      // No luck. Form the shared library name (incorporating build system
      // core version) and try using system-default search (installed, rpath,
      // etc).

      // @@ This is unfortunate: it would have been nice to do something
      //    similar to what we've done for exe{}. While libs{} is in the bin
      //    module, we could bring it in (we've done it for exe{}). The
      //    problems are: it is intertwined with its group (lib{}) and we
      //    don't have any mechanisms to deal with prefixes, only extensions.
      //
      const char* pfx;
      const char* sfx;
#if   defined(_WIN32)
      pfx = "build2-";    sfx = ".dll";
#elif defined(__APPLE__)
      pfx = "libbuild2-"; sfx = ".dylib";
#else
      pfx = "libbuild2-"; sfx = ".so";
#endif

      lib = path (pfx + mod + '-' + build_version_interface + sfx);
    }

    string sym (sanitize_identifier ("build2_" + mod + "_load"));

    // Note that we don't unload our modules since it's not clear what would
    // the benefit be.
    //
    module_load_function* r (nullptr);

#ifndef _WIN32
    // Use RTLD_NOW instead of RTLD_LAZY to both speed things up (we are going
    // to use this module now) and to detect any symbol mismatches.
    //
    if (void* h = dlopen (lib.string ().c_str (), RTLD_NOW | RTLD_GLOBAL))
    {
      r = function_cast<module_load_function*> (dlsym (h, sym.c_str ()));

      // I don't think we should ignore this even if optional.
      //
      if (r == nullptr)
        fail (loc) << "unable to lookup " << sym << " in build system module "
                   << mod << " (" << lib << "): " << dlerror ();
    }
    else if (!opt)
      fail (loc) << "unable to load build system module " << mod
                 << " (" << lib << "): " << dlerror ();
#else
    if (HMODULE h = LoadLibrary (lib.string ().c_str ()))
    {
      r = function_cast<module_load_function*> (
        GetProcAddress (h, sym.c_str ()));

      if (r == nullptr)
        fail (loc) << "unable to lookup " << sym << " in build system module "
                   << mod << " (" << lib << "): " << win32::last_error_msg ();
    }
    else if (!opt)
      fail (loc) << "unable to load build system module " << mod
                 << " (" << lib << "): " << win32::last_error_msg ();
#endif

    return r;
  }

  static const module_functions*
  find_module (scope& bs,
               const string& smod,
               const location& loc,
               bool boot,
               bool opt)
  {
    // Optional modules and submodules sure make this logic convoluted. So we
    // divide it into two parts: (1) find or insert an entry (for submodule
    // or, failed that, for the main module, the latter potentially NULL) and
    // (2) analyze the entry and issue diagnostics.
    //
    auto i (loaded_modules.find (smod)), e (loaded_modules.end ());

    if (i == e)
    {
      // If this is a submodule, get the main module name.
      //
      string mmod (smod, 0, smod.find ('.'));

      if (mmod != smod)
        i = loaded_modules.find (mmod);

      if (i == e)
      {
        module_load_function* f (import_module (bs, mmod, loc, boot, opt));

        if (f != nullptr)
        {
          // Enter all the entries noticing which one is our submodule. If
          // none are, then we notice the main module.
          //
          for (const module_functions* j (f ()); j->name != nullptr; ++j)
          {
            const string& n (j->name);

            auto p (loaded_modules.emplace (n, j));

            if (!p.second)
              fail (loc) << "build system submodule name " << n << " of main "
                         << "module " << mmod << " is already in use";

            if (n == smod || (i == e && n == mmod))
              i = p.first;
          }

          // We should at least have the main module.
          //
          if (i == e)
            fail (loc) << "invalid function list in build system module "
                       << mmod;
        }
        else
          i = loaded_modules.emplace (move (mmod), nullptr).first;
      }
    }

    // Now the iterator points to a submodule or to the main module, the
    // latter potentially NULL.
    //
    if (!opt)
    {
      if (i->second == nullptr)
      {
        fail (loc) << "unable to load build system module " << i->first;
      }
      else if (i->first != smod)
      {
        fail (loc) << "build system module " << i->first << " has no "
                   << "submodule " << smod;
      }
    }

    // Note that if the main module exists but has no such submodule, we
    // return NULL rather than fail (think of an older version of a module
    // that doesn't implement some extra functionality).
    //
    return i->second;
  }

  void
  boot_module (scope& rs, const string& mod, const location& loc)
  {
    // First see if this modules has already been booted for this project.
    //
    module_map& lm (rs.root_extra->modules);
    auto i (lm.find (mod));

    if (i != lm.end ())
    {
      module_state& s (i->second);

      // The only valid situation here is if the module has already been
      // bootstrapped.
      //
      assert (s.boot);
      return;
    }

    // Otherwise search for this module.
    //
    const module_functions& mf (
      *find_module (rs, mod, loc, true /* boot */, false /* optional */));

    if (mf.boot == nullptr)
      fail (loc) << "build system module " << mod << " should not be loaded "
                 << "during bootstrap";

    i = lm.emplace (mod,
                    module_state {true, false, mf.init, nullptr, loc}).first;
    i->second.first = mf.boot (rs, loc, i->second.module);

    rs.assign (rs.ctx.var_pool.rw (rs).insert (mod + ".booted")) = true;
  }

  bool
  init_module (scope& rs,
               scope& bs,
               const string& mod,
               const location& loc,
               bool opt,
               const variable_map& hints)
  {
    // First see if this modules has already been inited for this project.
    //
    module_map& lm (rs.root_extra->modules);
    auto i (lm.find (mod));
    bool f (i == lm.end ());

    if (f)
    {
      // Otherwise search for this module.
      //
      if (const module_functions* mf = find_module (
            rs, mod, loc, false /* boot */, opt))
      {
        if (mf->boot != nullptr)
          fail (loc) << "build system module " << mod << " should be loaded "
                     << "during bootstrap";

        i = lm.emplace (
          mod,
          module_state {false, false, mf->init, nullptr, loc}).first;
      }
    }
    else
    {
      module_state& s (i->second);

      if (s.boot)
      {
        s.boot = false;
        f = true; // This is a first call to init.
      }
    }

    // Note: pattern-typed in context.cxx:reset() as project-visibility
    // variables of type bool.
    //
    // We call the variable 'loaded' rather than 'inited' because it is
    // buildfile-visible (where we use the term "load a module"; see the note
    // on terminology above)
    //
    auto& vp (rs.ctx.var_pool.rw (rs));
    value& lv (bs.assign (vp.insert (mod + ".loaded")));
    value& cv (bs.assign (vp.insert (mod + ".configured")));

    bool l; // Loaded (initialized).
    bool c; // Configured.

    // Suppress duplicate init() calls for the same module in the same scope.
    //
    if (!lv.null)
    {
      assert (!cv.null);

      l = cast<bool> (lv);
      c = cast<bool> (cv);

      if (!opt)
      {
        if (!l)
          fail (loc) << "unable to load build system module " << mod;

        // We don't have original diagnostics. We could call init() again so
        // that it can issue it. But that means optional modules must be
        // prepared to be called again if configuring failed. Let's keep it
        // simple for now.
        //
        if (!c)
          fail (loc) << "build system module " << mod << " failed to "
                     << "configure";
      }
    }
    else
    {
      l = i != lm.end ();
      c = l && i->second.init (rs, bs, loc, i->second.module, f, opt, hints);

      lv = l;
      cv = c;
    }

    return l && c;
  }
}
