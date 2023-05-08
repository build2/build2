// file      : libbuild2/module.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/module.hxx>

#if !defined(BUILD2_BOOTSTRAP) && !defined(LIBBUILD2_STATIC_BUILD)
#  ifndef _WIN32
#    include <dlfcn.h>
#  else
#    include <libbutl/win32-utility.hxx>
#  endif
#endif

#include <libbuild2/file.hxx>  // import_*()
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/operation.hxx>
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
  mutex module_libraries_lock::mutex_;

  module_libraries_map module_libraries;

  void
  load_builtin_module (module_load_function* lf)
  {
    for (const module_functions* i (lf ()); i->name != nullptr; ++i)
      module_libraries.emplace (i->name, module_library {*i, dir_path ()});
  }

  // Sorted array of bundled modules (excluding core modules bundled with
  // libbuild2; see below).
  //
  static const char* bundled_modules[] = {
    "bash",
    "bin",
    "c",
    "cc",
    "cli",
    "cxx",
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

  // Note: also used by ad hoc recipes thus not static.
  //
  void
  create_module_context (context& ctx, const location& loc)
  {
    assert (ctx.module_context == nullptr);
    assert (*ctx.module_context_storage == nullptr);

    // Since we are using the same scheduler, it makes sense to reuse the
    // same global mutexes. Also disable nested module context for good
    // measure.
    //
    // The reserve values were picked experimentally by building libbuild2 and
    // adding a reasonable margin for future growth.
    //
    ctx.module_context_storage->reset (
      new context (*ctx.sched,
                   *ctx.mutexes,
                   *ctx.fcache,
                   nullopt,                  /* match_only */
                   false,                    /* no_external_modules */
                   false,                    /* dry_run */
                   ctx.no_diag_buffer,
                   ctx.keep_going,
                   ctx.global_var_overrides, /* cmd_vars */
                   context::reserves {
                     2500,                    /* targets */
                      900                     /* variables */
                   },
                   nullopt));                /* module_context */

    // We use the same context for building any nested modules that might be
    // required while building modules.
    //
    context& mctx (*(ctx.module_context = ctx.module_context_storage->get ()));
    mctx.module_context = &mctx;

    // Setup the context to perform update. In a sense we have a long-running
    // perform meta-operation batch (indefinite, in fact, since we never call
    // the meta-operation's *_post() callbacks) in which we periodically
    // execute update operations.
    //
    // Note that we perform each build in a separate update operation. Failed
    // that, if the same target is update twice (which may happen with ad hoc
    // recipes) we will see the old state.
    //
    if (mo_perform.meta_operation_pre != nullptr)
      mo_perform.meta_operation_pre (mctx, {} /* parameters */, loc);

    mctx.current_meta_operation (mo_perform);

    if (mo_perform.operation_pre != nullptr)
      mo_perform.operation_pre (mctx, {} /* parameters */, update_id);
  }

  // Note: also used by ad hoc recipes thus not static.
  //
  const target&
  update_in_module_context (context& ctx, const scope& rs, names tgt,
                            const location& loc, const path& bf)
  {
    // New update operation.
    //
    assert (op_update.operation_pre == nullptr &&
            op_update.operation_post == nullptr);

    ctx.module_context->current_operation (op_update);

    // Un-tune the scheduler.
    //
    // Note that we can only do this if we are running serially because
    // otherwise we cannot guarantee the scheduler is idle (we could have
    // waiting threads from the outer context). This is fine for now since the
    // only two tuning level we use are serial and full concurrency. (Turns
    // out currently we don't really need this: we will always be called
    // during load or match phases and we always do parallel match; but let's
    // keep it in case things change. Actually, we may need it, if the
    // scheduler was started up in a tuned state, like in bpkg).
    //
    auto sched_tune (ctx.sched->serial ()
                     ? scheduler::tune_guard (*ctx.sched, 0)
                     : scheduler::tune_guard ());

    // Remap verbosity level 0 to 1 unless we were requested to be silent.
    // Failed that, we may have long periods of seemingly nothing happening
    // while we quietly update the module, which may look like things have
    // hung up.
    //
    // @@ CTX: modifying global verbosity level won't work if we have multiple
    //         top-level contexts running in parallel.
    //
    auto verbg = make_guard (
      [z = !silent && verb == 0 ? (verb = 1, true) : false] ()
      {
        if (z)
          verb = 0;
      });

    // Note that for now we suppress progress since it would clash with the
    // progress of what we are already doing (maybe in the future we can do
    // save/restore but then we would need some sort of diagnostics that we
    // have switched to another task).
    //
    action a (perform_update_id);
    action_targets tgs;

    mo_perform.search  ({},      /* parameters */
                        rs,      /* root scope */
                        rs,      /* base scope */
                        bf,      /* buildfile */
                        rs.find_target_key (tgt, loc),
                        loc,
                        tgs);

    mo_perform.match   ({},      /* parameters */
                        a,
                        tgs,
                        1,       /* diag (failures only) */
                        false    /* progress */);

    mo_perform.execute ({},      /* parameters */
                        a,
                        tgs,
                        1,       /* diag (failures only) */
                        false    /* progress */);

    assert (tgs.size () == 1);
    return tgs[0].as<target> ();
  }

  // Note: also used by ad hoc recipes thus not static.
  //
#if !defined(BUILD2_BOOTSTRAP) && !defined(LIBBUILD2_STATIC_BUILD)
  pair<void* /* handle */, void* /* symbol */>
  load_module_library (const path& lib, const string& sym, string& err)
  {
    // Note that we don't unload our modules since it's not clear what would
    // the benefit be.
    //
    void* h (nullptr);
    void* s (nullptr);

#ifndef _WIN32
    // Use RTLD_NOW instead of RTLD_LAZY to both speed things up (we are going
    // to use this module now) and to detect any symbol mismatches.
    //
    if ((h = dlopen (lib.string ().c_str (), RTLD_NOW | RTLD_GLOBAL)))
    {
      s = dlsym (h, sym.c_str ());

      if (s == nullptr)
        err = dlerror ();
    }
    else
      err = dlerror ();
#else
    if (HMODULE m = LoadLibrary (lib.string ().c_str ()))
    {
      h = static_cast<void*> (m);
      s = function_cast<void*> (GetProcAddress (m, sym.c_str ()));

      if (s == nullptr)
        err = win32::last_error_msg ();
    }
    else
      err = win32::last_error_msg ();
#endif

    return make_pair (h, s);
  }
#else
  pair<void*, void*>
  load_module_library (const path&, const string&, string&)
  {
    return pair<void*, void*> (nullptr, nullptr);
  }
#endif

  // Return the module functions as well as the module project directory or
  // empty if not imported from project. Return {nullptr, nullopt} if not
  // found.
  //
  // The dry-run mode only calls import_search() and always returns NULL for
  // module functions (see below for background).
  //
  static pair<module_load_function*, optional<dir_path>>
  import_module (
#if defined(BUILD2_BOOTSTRAP) || defined(LIBBUILD2_STATIC_BUILD)
    bool,
    scope&,
#else
    bool dry_run,
    scope& bs,
#endif
    const string& mod,
    const location& loc,
#if defined(BUILD2_BOOTSTRAP) || defined(LIBBUILD2_STATIC_BUILD)
    bool,
#else
    bool boot,
#endif
    bool opt)
  {
    tracer trace ("import_module");

    pair<module_load_function*, optional<dir_path>> r (nullptr, nullopt);

    // Take care of core modules that are bundled with libbuild2 in case they
    // are not pre-loaded by the driver.
    //
    if      (mod == "config")  r.first = &config::build2_config_load;
    else if (mod == "dist")    r.first = &dist::build2_dist_load;
    else if (mod == "install") r.first = &install::build2_install_load;
    else if (mod == "test")    r.first = &test::build2_test_load;

    if (r.first != nullptr)
    {
      r.second = dir_path ();
      return r;
    }

    // No dynamic loading of build system modules during bootstrap or if
    // statically-linked..
    //
#if defined(BUILD2_BOOTSTRAP) || defined(LIBBUILD2_STATIC_BUILD)
    if (!opt)
    {
      fail (loc) << "unknown build system module " << mod <<
#ifdef BUILD2_BOOTSTRAP
        info << "running bootstrap build system";
#else
        info << "running statically-linked build system";
#endif
    }
#else
    context& ctx (bs.ctx);

    bool bundled (bundled_module (mod));

    // Note that importing external modules during bootstrap is problematic
    // since we haven't loaded config.build nor entered non-global variable
    // overrides. We used to just not support external modules that require
    // bootstrapping but that proved to restrictive. So now we allow such
    // modules and the following mechanisms can be used to make things work
    // in various situations:
    //
    // 1. Module is installed.
    //
    //    This covers both user-installed modules as well as the module's
    //    *-tests in our CI setup (where we install the module next to the
    //    build system).
    //
    // 2. Module is specified with global !config.import.<module> override.
    //
    //    This covers development (where the override can be specified in the
    //    default options file) and could cover imports from the bpkg-managed
    //    host configuration if we use global overrides to connect things
    //    (which feels correct; we shouldn't have multiple host configurations
    //    in any given build).
    //
    // One case that is not straightforward is using the module in testscript-
    // generated tests (e.g., in module's *-tests). This will work in CI
    // (installed module) and in development provided !config.import.* is
    // specified in the default options file (and we haven't suppressed it).
    //
    // In fact, this is not specific to modules that require bootstrapping; we
    // have the same config.import.* propagation problem from, say, *-tests's
    // config.build. To make other cases work (config.import.* specified in
    // places other than the default options file) we would have to propagate
    // things explicitly. So for now the thinking is that one shouldn't write
    // such tests except in controlled cases (e.g., module's *-tests).
    //
    // And another case is the bdep-sync hook which also doesn't have the
    // global overrides propagated to it.
    //
    // And it turns out the story does not end here: without an external
    // module we cannot do info or dist. So to support this we now allow
    // skipping of loading of external modules (for dist this is only part of
    // the solution with the other part being the bootstrap mode). While no
    // doubt a hack, it feels like this is the time to cut of this complexity
    // escalation. Essentially, we are saying external module that require
    // bootstrap must be prepared to be skipped if the project is only being
    // bootstrapped. Note also that the fact that a module boot was skipped
    // can be detected by checking the module's *.booted variable. In case of
    // a skip it will be false, as opposed to true if the module was booted
    // and undefined if the module was not mentioned.
    //
    if (boot && !bundled && ctx.no_external_modules)
      return r; // NULL

    // See if we can import a target for this module.
    //
    path lib;

    // If this is a top-level module update, then we use the nested context.
    // If, however, this is a nested module update (i.e., a module required
    // while updating a module), then we reuse the same module context.
    //
    // If you are wondering why don't we always use the top-level context, the
    // reason is that it might be running a different meta/operation (say,
    // configure or clean); with the nested context we always know it is
    // perform update.
    //
    // And the reason for not simply creating a nested context for each nested
    // module update is due to the no-overlap requirement of contexts: while
    // we can naturally expect the top-level project(s) and the modules they
    // require to be in separate configurations that don't shared anything,
    // the same does not hold for build system modules. In fact, it would be
    // natural to have a single build configuration for all of them and they
    // could plausibly share some common libraries.
    //
    bool nested (ctx.module_context == &ctx);

    // If this is one of the bundled modules, the project name is build2,
    // otherwise -- libbuild2-<mod>.
    //
    project_name proj;
    try
    {
      proj = project_name (bundled ? "build2" : "libbuild2-" + mod);
    }
    catch (const invalid_argument& e)
    {
      fail (loc) << "invalid build system module '" << mod << "': " << e;
    }

    // The target we are looking for is <prj>%libs{build2-<mod>}.
    //
    // We only search in subprojects if this is a nested module update
    // (remember, if it's top-level, then it must be in an isolated
    // configuration).
    //
    pair<name, optional<dir_path>> ir (
      import_search (bs,
                     name (proj, dir_path (), "libs", "build2-" + mod),
                     opt,
                     nullopt  /* metadata    */,
                     nested   /* subprojects */,
                     loc));

    if (ir.first.empty ())
    {
      assert (opt);
      return r; // NULL
    }

    if (ir.second)
    {
      // What if a module is specified with config.import.<mod>.<lib>.libs?
      // Note that this could still be a project-qualified target.
      //
      // Note: we now return an empty directory to mean something else.
      //
      if (ir.second->empty ())
        fail (loc) << "direct module target importation not yet supported";

      // We found the module as a target in a project. Now we need to update
      // the target (which will also give us the shared library path).
      //
      l5 ([&]{trace << "found " << ir.first << " in " << *ir.second;});
    }

    if (dry_run)
    {
      r.second = ir.second ? move (*ir.second) : dir_path ();
      return r;
    }

    if (ir.second)
    {
      r.second = *ir.second;

      // Create the build context if necessary.
      //
      if (ctx.module_context == nullptr)
      {
        if (!ctx.module_context_storage)
          fail (loc) << "unable to update build system module " << mod <<
            info << "building of build system modules is disabled";

        create_module_context (ctx, loc);
      }

      // Inherit module_libraries lock from the outer context.
      //
      ctx.module_context->modules_lock = ctx.modules_lock;

      // Clear current project's environment and "switch" to the module
      // context, including entering a scheduler sub-phase.
      //
      auto_thread_env penv (nullptr);
      context& ctx (*bs.ctx.module_context);
      scheduler::phase_guard pg (*ctx.sched);

      // Load the imported project in the module context.
      //
      pair<names, const scope&> lr (
        import_load (ctx, move (ir), false /* metadata */, loc));

      l5 ([&]{trace << "loaded " << lr.first;});

      // What happens next depends on whether this is a top-level or nested
      // module update.
      //
      if (nested)
      {
        // This could be initial or exclusive load.
        //
        // @@ TODO: see the ad hoc recipe case as a reference.
        //
        fail (loc) << "nested build system module updates not yet supported";
      }
      else
      {
        const target* l;
        {
          // Cutoff the existing diagnostics stack and push our own entry.
          //
          diag_frame::stack_guard diag_cutoff (nullptr);

          auto df = make_diag_frame (
            [&loc, &mod] (const diag_record& dr)
            {
              dr << info (loc) << "while loading build system module " << mod;
            });

          l = &update_in_module_context (
            ctx, lr.second, move (lr.first),
            loc, path ());
        }

        if (!l->is_a ("libs"))
          fail (loc) << "wrong export from build system module " << mod;

        lib = l->as<file> ().path ();

        l5 ([&]{trace << "updated " << lib;});
      }

      ctx.modules_lock = nullptr; // For good measure.
    }
    else
    {
      r.second = dir_path ();

      // No module project found. Form the shared library name (incorporating
      // build system core version) and try using system-default search
      // (installed, rpath, etc).

      // @@ This is unfortunate: it would have been nice to do something
      //    similar to what we've done for exe{}. While libs{} is in the bin
      //    module, we could bring it in (we've done it for exe{}). The
      //    problems are: it is intertwined with its group (lib{}) and we
      //    don't have any mechanisms to deal with prefixes, only extensions.
      //
      const char* pfx;
      const char* sfx;
#if   defined(__MINGW32__)
      pfx = "libbuild2-"; sfx = ".dll";
#elif defined(_WIN32)
      pfx = "build2-";    sfx = ".dll";
#elif defined(__APPLE__)
      pfx = "libbuild2-"; sfx = ".dylib";
#else
      pfx = "libbuild2-"; sfx = ".so";
#endif

      lib = path (pfx + mod + '-' + build_version_interface + sfx);

      l5 ([&]{trace << "system-default search for " << lib;});
    }

    // The build2_<mod>_load() symbol name.
    //
    string sym (sanitize_identifier ("build2_" + mod + "_load"));

    string err;
    pair<void*, void*> hs (load_module_library (lib, sym, err));

    if (hs.first != nullptr)
    {
      // I don't think we should ignore this even if the module is optional.
      //
      if (hs.second == nullptr)
        fail (loc) << "unable to lookup " << sym << " in build system module "
                   << mod << " (" << lib << "): " << err;

      r.first = function_cast<module_load_function*> (hs.second);
    }
    else if (!opt)
    {
      // Add import suggestion similar to import phase 2.
      //
      fail (loc) << "unable to load build system module " << mod << " ("
                 << lib << "): " << err <<
        info     << "use config.import." << proj.variable () << " command "
                 << "line variable to specify its project out_root";
    }
    else
    {
      r.second = nullopt;
      l5 ([&]{trace << "unable to load " << lib << ": " << err;});
    }

#endif // BUILD2_BOOTSTRAP || LIBBUILD2_STATIC_BUILD

    return r;
  }

  static const module_functions*
  find_module (scope& bs,
               const string& smod,
               const location& loc,
               bool boot,
               bool opt)
  {
    tracer trace ("find_module");

    // If this is a submodule, get the main module name.
    //
    string mmod (smod, 0, smod.find ('.'));

    // We have a somewhat strange two-level caching in imported_modules
    // and module_libraries in order to achieve the following:
    //
    // 1. Correctly handle cases where a module can be imported from one
    //    project but not the other.
    //
    // 2. Make sure that for each project that imports the module we actually
    //    call import_search() in order to mark any config.import.* as used.
    //
    // 3. Make sure that all the projects import the same module.
    //
    scope& rs (*bs.root_scope ());

    const string* mod;
    const module_functions* fun;

    // First check the project's imported_modules in case this (main) module
    // is known to be not found.
    //
    auto j (rs.root_extra->imported_modules.find (mmod));
    auto je (rs.root_extra->imported_modules.end ());

    if (j != je && !j->found)
    {
      mod = &mmod;
      fun = nullptr;
    }
    else
    {
      // Note that we hold the lock for the entire time it takes to build a
      // module.
      //
      module_libraries_lock lock (bs.ctx);

      // Optional modules and submodules sure make this logic convoluted. So
      // we divide it into two parts: (1) find or insert an entry (for
      // submodule or, failed that, for the main module) and (2) analyze the
      // entry and issue diagnostics.
      //
      auto i (module_libraries.find (smod));
      auto ie (module_libraries.end ());

      bool imported (false);
      if (i == ie)
      {
        if (mmod != smod)
          i = module_libraries.find (mmod);

        if (i == ie)
        {
          pair<module_load_function*, optional<dir_path>> ir (
            import_module (false /* dry_run */, bs, mmod, loc, boot, opt));

          if (module_load_function* f = ir.first)
          {
            // Enter all the entries noticing which one is our submodule. If
            // none are, then we notice the main module.
            //
            for (const module_functions* j (f ()); j->name != nullptr; ++j)
            {
              const string& n (j->name);

              l5 ([&]{trace << "registering " << n;});

              bool main (n == mmod);

              auto p (module_libraries.emplace (
                        n,
                        module_library {
                          *j,
                          main ? move (*ir.second) : dir_path ()}));

              if (!p.second)
                fail (loc) << "build system submodule name " << n << " of main "
                           << "module " << mmod << " is already in use";

              // Note: this assumes the main module is last.
              //
              if (n == smod || (main && i == ie))
                i = p.first;
            }

            // We should at least have the main module.
            //
            if (i == ie)
              fail (loc) << "invalid function list in build system module "
                         << mmod;
          }

          imported = true;
        }
      }

      // Now the iterator points to a submodule or to the main module, or to
      // end if neither is found.
      //
      assert (j == je || i != ie); // Cache state consistecy sanity check.

      if (i != ie)
      {
        // Note: these should remain stable after we release the lock.
        //
        mod = &i->first;
        fun = &i->second.functions.get ();

        // If this project hasn't imported this main module and we found the
        // entry in the cache, then we have to perform the import_search()
        // part of import_module() in order to cover items (2) and (3) above.
        //
        // There is one nuance: omit this for bundled modules since it's
        // possible to first import them ad hoc and then, if we call
        // import_search() again, to find them differently (e.g., as a
        // subproject).
        //
        if (j == je && !imported && !bundled_module (mmod))
        {
          pair<module_load_function*, optional<dir_path>> ir (
            import_module (true /* dry_run */, bs, mmod, loc, boot, opt));

          if (ir.second)
          {
            if (i->first != mmod)
            {
              i = module_libraries.find (mmod);
              assert (i != ie); // Has to be there.
            }

            const dir_path& cd (*ir.second);
            const dir_path& pd (i->second.import_path);

            if (cd != pd)
            {
              fail (loc) << "inconsistent build system module " << mmod
                         << " importation" <<
                info << rs << " imports it as "
                     << (cd.empty () ? "ad hoc" : cd.representation ().c_str ()) <<
                info << "previously imported as "
                     << (pd.empty () ? "ad hoc" : pd.representation ().c_str ());
            }
          }
          else
          {
            // This module is not found from this project.
            //
            mod = &mmod;
            fun = nullptr;
          }
        }
      }
      else
      {
        mod = &mmod;
        fun = nullptr;
      }
    }

    // Cache the result in imported_modules if necessary.
    //
    if (j == je)
      rs.root_extra->imported_modules.push_back (
        module_import {mmod, fun != nullptr});

    // Reduce skipped external module to optional.
    //
    if (boot && fun == nullptr)
      opt = true;

    // Handle optional.
    //
    if (fun == nullptr)
    {
      if (!opt)
        fail (loc) << "unable to load build system module " << *mod;
    }
    else if (*mod != smod)
    {
      if (!opt)
        fail (loc) << "build system module " << *mod << " has no "
                   << "submodule " << smod;
      else
      {
        // Note that if the main module exists but has no such submodule, we
        // return NULL rather than fail (think of an older version of a module
        // that doesn't implement some extra functionality).
        //
        fun = nullptr;
      }
    }

    return fun;
  }

  void
  boot_module (scope& rs, const string& mod, const location& loc)
  {
    // First see if this modules has already been booted for this project.
    //
    module_state_map& lm (rs.root_extra->loaded_modules);
    auto i (lm.find (mod));

    if (i != lm.end ())
    {
      // The only valid situation here is if the module has already been
      // bootstrapped.
      //
      assert (i->boot_init);
      return;
    }

    // Otherwise search for this module.
    //
    // Note that find_module() may return NULL in case of a skipped external
    // module.
    //
    const module_functions* mf (
      find_module (rs, mod, loc, true /* boot */, false /* optional */));

    if (mf != nullptr)
    {
      if (mf->boot == nullptr)
        fail (loc) << "build system module " << mod << " should not be loaded "
                   << "during bootstrap";

      lm.push_back (
        module_state {loc, mod, nullptr, mf->init, nullptr, nullopt});
      i = lm.end () - 1;

      module_boot_extra e {nullptr, nullptr, module_boot_init::before};

      // Note: boot() can load additional modules invalidating the iterator.
      //
      size_t j (i - lm.begin ());
      mf->boot (rs, loc, e);
      i = lm.begin () + j;

      if (e.module != nullptr)
        i->module = move (e.module);

      i->boot_post = e.post;
      i->boot_init = e.init;
    }

    rs.assign (rs.var_pool (true).insert (mod + ".booted")) = (mf != nullptr);
  }

  void
  boot_post_module (scope& rs, module_state& s)
  {
    module_boot_post_extra e {s.module, *s.boot_init};

    // Note: boot_post() should be loading any additional modules.
    //
    s.boot_post (rs, s.loc, e);

    if (e.module != s.module)
    {
      assert (s.module == nullptr);
      s.module = move (e.module);
    }

    s.boot_init = e.init;
  }

  module_state*
  init_module (scope& rs,
               scope& bs,
               const string& mod,
               const location& loc,
               bool opt,
               const variable_map& hints)
  {
    // First see if this modules has already been inited for this project.
    //
    module_state_map& lm (rs.root_extra->loaded_modules);
    auto i (lm.find (mod));
    bool f (i == lm.end ());

    if (f)
    {
      // Otherwise search for this module.
      //
      if (const module_functions* mf = find_module (
            bs, mod, loc, false /* boot */, opt))
      {
        if (mf->boot != nullptr)
          fail (loc) << "build system module " << mod << " should be loaded "
                     << "during bootstrap";

        lm.push_back (
          module_state {loc, mod, nullptr, mf->init, nullptr, nullopt});
        i = lm.end () - 1;
      }
    }
    else
    {
      module_state& s (*i);

      if (s.boot_init)
      {
        s.boot_init = nullopt;
        f = true; // This is a first call to init.
      }
    }

    // Note: pattern-typed in context ctor as project visibility variables of
    // type bool.
    //
    // We call the variable 'loaded' rather than 'inited' because it is
    // buildfile-visible (where we use the term "load a module"; see the note
    // on terminology above)
    //
    auto& vp (rs.var_pool (true));
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

      if ((c = l))
      {
        module_init_extra e {i->module, hints};

        // Note: init() can load additional modules invalidating the iterator.
        //
        size_t j (i - lm.begin ());
        c = i->init (rs, bs, loc, f, opt, e);
        i = lm.begin () + j;

        if (e.module != i->module)
        {
          assert (i->module == nullptr);
          i->module = move (e.module);
        }
      }

      lv = l;
      cv = c;
    }

    return l && c ? &*i : nullptr;
  }

  // @@ TODO: This is a bit of a fuzzy mess:
  //
  //    - The .loaded variable check: it's not clear if init_module()
  //      already has this semantics?
  //
  //    - Why do we use variable instead of the module map entry? Probably
  //      because of optional. Also entry present if only booted. Need to be
  //      careful here. Also root vs base!
  //
  // Note that it would have been nice to keep these inline but we need the
  // definition of scope for the variable lookup.
  //
  optional<shared_ptr<module>>
  load_module (scope& rs,
               scope& bs,
               const string& name,
               const location& loc,
               bool opt,
               const variable_map& hints)
  {
    if (cast_false<bool> (bs[name + ".loaded"]))
    {
      if (cast_false<bool> (bs[name + ".configured"]))
        return rs.root_extra->loaded_modules.find (name)->module;
    }
    else
    {
      if (module_state* ms = init_module (rs, bs, name, loc, opt, hints))
        return ms->module;
    }

    return nullopt;
  }

  shared_ptr<module>
  load_module (scope& rs,
               scope& bs,
               const string& name,
               const location& loc,
               const variable_map& hints)
  {
    //@@ TODO: shouldn't we also check for configured? What if the previous
    //   attempt to load it was optional?

    return cast_false<bool> (bs[name + ".loaded"])
      ? rs.root_extra->loaded_modules.find (name)->module
      : init_module (rs, bs, name, loc, false /* optional */, hints)->module;
  }
}
