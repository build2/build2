// file      : libbuild2/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/rule.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/build/script/parser.hxx>
#include <libbuild2/build/script/runner.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // rule (vtable)
  //
  rule::
  ~rule ()
  {
  }

  // file_rule
  //
  // Note that this rule is special. It is the last, fallback rule. If
  // it doesn't match, then no other rule can possibly match and we have
  // an error. It also cannot be ambigious with any other rule. As a
  // result the below implementation bends or ignores quite a few rules
  // that normal implementations should follow. So you probably shouldn't
  // use it as a guide to implement your own, normal, rules.
  //
  bool file_rule::
  match (action a, target& t, const string&) const
  {
    tracer trace ("file_rule::match");

    // While strictly speaking we should check for the file's existence
    // for every action (because that's the condition for us matching),
    // for some actions this is clearly a waste. Say, perform_clean: we
    // are not doing anything for this action so not checking if the file
    // exists seems harmless.
    //
    switch (a)
    {
    case perform_clean_id:
      return true;
    default:
      {
        // While normally we shouldn't do any of this in match(), no other
        // rule should ever be ambiguous with the fallback one and path/mtime
        // access is atomic. In other words, we know what we are doing but
        // don't do this in normal rules.

        // First check the timestamp. This takes care of the special "trust
        // me, this file exists" situations (used, for example, for installed
        // stuff where we know it's there, just not exactly where).
        //
        mtime_target& mt (t.as<mtime_target> ());

        timestamp ts (mt.mtime ());

        if (ts != timestamp_unknown)
          return ts != timestamp_nonexistent;

        // Otherwise, if this is not a path_target, then we don't match.
        //
        path_target* pt (mt.is_a<path_target> ());
        if (pt == nullptr)
          return false;

        const path* p (&pt->path ());

        // Assign the path.
        //
        if (p->empty ())
        {
          // Since we cannot come up with an extension, ask the target's
          // derivation function to treat this as a prerequisite (just like in
          // search_existing_file()).
          //
          if (pt->derive_extension (true) == nullptr)
          {
            l4 ([&]{trace << "no default extension for target " << *pt;});
            return false;
          }

          p = &pt->derive_path ();
        }

        ts = mtime (*p);
        pt->mtime (ts);

        if (ts != timestamp_nonexistent)
          return true;

        l4 ([&]{trace << "no existing file for target " << *pt;});
        return false;
      }
    }
  }

  recipe file_rule::
  apply (action a, target& t) const
  {
    // Update triggers the update of this target's prerequisites so it would
    // seem natural that we should also trigger their cleanup. However, this
    // possibility is rather theoretical so until we see a real use-case for
    // this functionality, we simply ignore the clean operation.
    //
    if (a.operation () == clean_id)
      return noop_recipe;

    // If we have no prerequisites, then this means this file is up to date.
    // Return noop_recipe which will also cause the target's state to be set
    // to unchanged. This is an important optimization on which quite a few
    // places that deal with predominantly static content rely.
    //
    if (!t.has_group_prerequisites ()) // Group as in match_prerequisites().
      return noop_recipe;

    // Match all the prerequisites.
    //
    match_prerequisites (a, t);

    // Note that we used to provide perform_update() which checked that this
    // target is not older than any of its prerequisites. However, later we
    // realized this is probably wrong: consider a script with a testscript as
    // a prerequisite; chances are the testscript will be newer than the
    // script and there is nothing wrong with that.
    //
    return default_recipe;
  }

  const file_rule file_rule::instance;

  // alias_rule
  //
  bool alias_rule::
  match (action, target&, const string&) const
  {
    return true;
  }

  recipe alias_rule::
  apply (action a, target& t) const
  {
    // Inject dependency on our directory (note: not parent) so that it is
    // automatically created on update and removed on clean.
    //
    inject_fsdir (a, t, false);

    match_prerequisites (a, t);
    return default_recipe;
  }

  const alias_rule alias_rule::instance;

  // fsdir_rule
  //
  bool fsdir_rule::
  match (action, target&, const string&) const
  {
    return true;
  }

  recipe fsdir_rule::
  apply (action a, target& t) const
  {
    // Inject dependency on the parent directory. Note that it must be first
    // (see perform_update_direct()).
    //
    inject_fsdir (a, t);

    match_prerequisites (a, t);

    switch (a)
    {
    case perform_update_id: return &perform_update;
    case perform_clean_id: return &perform_clean;
    default: assert (false); return default_recipe;
    }
  }

  static bool
  fsdir_mkdir (const target& t, const dir_path& d)
  {
    // Even with the exists() check below this can still be racy so only print
    // things if we actually did create it (similar to build2::mkdir()).
    //
    auto print = [&t, &d] ()
    {
      if (verb >= 2)
        text << "mkdir " << d;
      else if (verb && t.ctx.current_diag_noise)
        text << "mkdir " << t;
    };

    // Note: ignoring the dry_run flag.
    //
    mkdir_status ms;

    try
    {
      ms = try_mkdir (d);
    }
    catch (const system_error& e)
    {
      print ();
      fail << "unable to create directory " << d << ": " << e << endf;
    }

    if (ms == mkdir_status::success)
    {
      print ();
      return true;
    }

    return false;
  }

  target_state fsdir_rule::
  perform_update (action a, const target& t)
  {
    target_state ts (target_state::unchanged);

    // First update prerequisites (e.g. create parent directories) then create
    // this directory.
    //
    // @@ outer: should we assume for simplicity its only prereqs are fsdir{}?
    //
    if (!t.prerequisite_targets[a].empty ())
      ts = straight_execute_prerequisites (a, t);

    // The same code as in perform_update_direct() below.
    //
    const dir_path& d (t.dir); // Everything is in t.dir.

    // Generally, it is probably correct to assume that in the majority of
    // cases the directory will already exist. If so, then we are going to get
    // better performance by first checking if it indeed exists. See
    // butl::try_mkdir() for details.
    //
    // @@ Also skip prerequisites? Can't we return noop in apply?
    //
    if (!exists (d) && fsdir_mkdir (t, d))
      ts |= target_state::changed;

    return ts;
  }

  void fsdir_rule::
  perform_update_direct (action a, const target& t)
  {
    // First create the parent directory. If present, it is always first.
    //
    const target* p (t.prerequisite_targets[a].empty ()
                     ? nullptr
                     : t.prerequisite_targets[a][0]);

    if (p != nullptr && p->is_a<fsdir> ())
      perform_update_direct (a, *p);

    // The same code as in perform_update() above.
    //
    const dir_path& d (t.dir);

    if (!exists (d))
      fsdir_mkdir (t, d);
  }

  target_state fsdir_rule::
  perform_clean (action a, const target& t)
  {
    // The reverse order of update: first delete this directory, then clean
    // prerequisites (e.g., delete parent directories).
    //
    // Don't fail if we couldn't remove the directory because it is not empty
    // (or is current working directory). In this case rmdir() will issue a
    // warning when appropriate.
    //
    target_state ts (rmdir (t.dir, t, t.ctx.current_diag_noise ? 1 : 2)
                     ? target_state::changed
                     : target_state::unchanged);

    if (!t.prerequisite_targets[a].empty ())
      ts |= reverse_execute_prerequisites (a, t);

    return ts;
  }

  const fsdir_rule fsdir_rule::instance;

  // noop_rule
  //
  bool noop_rule::
  match (action, target&, const string&) const
  {
    return true;
  }

  recipe noop_rule::
  apply (action, target&) const
  {
    return noop_recipe;
  }

  const noop_rule noop_rule::instance;

  // adhoc_rule
  //
  const dir_path adhoc_rule::recipes_build_dir ("recipes.out");

  bool adhoc_rule::
  match (action a, target& t, const string& h, optional<action> fallback) const
  {
    return !fallback && match (a, t, h);
  }

  bool adhoc_rule::
  match (action, target&, const string&) const
  {
    return true;
  }

  // Scope operation callback that cleans up recipe builds.
  //
  target_state adhoc_rule::
  clean_recipes_build (action, const scope& rs, const dir&)
  {
    context& ctx (rs.ctx);

    const dir_path& out_root (rs.out_path ());

    dir_path d (out_root / rs.root_extra->build_dir / recipes_build_dir);

    if (exists (d))
    {
      if (rmdir_r (ctx, d))
      {
        // Clean up build/ if it also became empty (e.g., in case of a build
        // with a transient configuration).
        //
        d = out_root / rs.root_extra->build_dir;
        if (empty (d))
          rmdir (ctx, d);

        return target_state::changed;
      }
    }

    return target_state::unchanged;
  }

  // adhoc_script_rule
  //
  void adhoc_script_rule::
  dump (ostream& os, string& ind) const
  {
    // Do we need the header?
    //
    // @@ TODO: for now we dump it as an attribute whether it was specified or
    //    derived from the script. Maybe that's ok?
    //
    if (script.diag)
    {
      os << ind << '%';

      if (script.diag)
      {
        os << " [";
        os << "diag=";
        to_stream (os, name (*script.diag), true /* quote */, '@');
        os << ']';
      }

      os << endl;
    }

    os << ind << string (braces, '{') << endl;
    ind += "  ";
    script::dump (os, ind, script.lines);
    ind.resize (ind.size () - 2);
    os << ind << string (braces, '}');
  }

  bool adhoc_script_rule::
  match (action a, target& t, const string&, optional<action> fb) const
  {
    if (!fb)
      ;
    // If this is clean for a file target and we are supplying the update,
    // then we will also supply the standard clean.
    //
    else if (a   == perform_clean_id  &&
             *fb == perform_update_id &&
             t.is_a<file> ())
      ;
    else
      return false;

    // It's unfortunate we have to resort to this but we need to remember this
    // in apply().
    //
    t.data (fb.has_value ());

    return true;
  }

  recipe adhoc_script_rule::
  apply (action a, target& t) const
  {
    // Derive file names for the target and its ad hoc group members, if any.
    //
    for (target* m (&t); m != nullptr; m = m->adhoc_member)
    {
      if (auto* p = m->is_a<path_target> ())
        p->derive_path ();
    }

    // Inject dependency on the output directory.
    //
    // We do it always instead of only if one of the targets is path-based in
    // case the recipe creates temporary files or some such.
    //
    inject_fsdir (a, t);

    // Match prerequisites.
    //
    match_prerequisite_members (a, t);

    // See if we are providing the standard clean as a fallback.
    //
    if (t.data<bool> ())
      return &perform_clean_depdb;

    // For update inject dependency on the tool target(s).
    //
    // @@ We could see that it's a target and do it but not sure if we should
    //    bother. We dropped this idea of implicit targets in tests. Maybe we
    //    should verify path assigned, like we do there? I think we will have
    //    to.
    //
    // if (a == perform_update_id)
    //  inject (a, t, tgt);

    if (a == perform_update_id && t.is_a<file> ())
    {
      return [this] (action a, const target& t)
      {
        return perform_update_file (a, t);
      };
    }
    else
    {
      return [this] (action a, const target& t)
      {
        return default_action (a, t);
      };
    }
  }

  target_state adhoc_script_rule::
  perform_update_file (action a, const target& xt) const
  {
    tracer trace ("adhoc_rule::perform_update_file");

    context& ctx (xt.ctx);

    const file& t (xt.as<file> ());
    const path& tp (t.path ());

    // Update prerequisites and determine if any of them render this target
    // out-of-date.
    //
    timestamp mt (t.load_mtime ());
    optional<target_state> ps (execute_prerequisites (a, t, mt));

    bool update (!ps);

    // We use depdb to track changes to the script itself, input/output file
    // names, tools, etc.
    //
    depdb dd (tp + ".d");
    {
      // First should come the rule name/version.
      //
      if (dd.expect ("adhoc 1") != nullptr)
        l4 ([&]{trace << "rule mismatch forcing update of " << t;});

      // Then the script checksum.
      //
      // Ideally, to detect changes to the script semantics, we would hash the
      // text with all the variables expanded but without executing any
      // commands. In practice, this is easier said than done (think the set
      // builtin that receives output of a command that modifies the
      // filesystem).
      //
      // So as the next best thing we are going to hash the unexpanded text as
      // well as values of all the variables expanded in it (which we get as a
      // side effect of pre-parsing the script). This approach has a number of
      // drawbacks:
      //
      // - We can't handle computed variable names (e.g., $($x ? X : Y)).
      //
      // - We may "overhash" by including variables that are actually
      //   script-local.
      //
      // - There are functions like $install.resolve() with result based on
      //   external (to the script) information.
      //
      if (dd.expect (checksum) != nullptr)
        l4 ([&]{trace << "recipe text change forcing update of " << t;});

      // For each variable hash its name, undefined/null/non-null indicator,
      // and the value if non-null.
      //
      // Note that this excludes the special $< and $> variables which we
      // handle below.
      //
      {
        sha256 cs;
        names storage;

        for (const string& n: script.vars)
        {
          cs.append (n);

          lookup l;

          if (const variable* var = ctx.var_pool.find (n))
            l = t[var];

          cs.append (!l.defined () ? '\x1' : l->null ? '\x2' : '\x3');

          if (l)
          {
            storage.clear ();
            names_view ns (reverse (*l, storage));

            for (const name& n: ns)
              to_checksum (cs, n);
          }
        }

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "recipe variable change forcing update of " << t;});
      }

      // Target and prerequisite sets ($> and $<).
      //
      // How should we hash them? We could hash them as target names (i.e.,
      // the same as the $>/< content) or as paths (only for path-based
      // targets). While names feel more general, they are also more expensive
      // to compute. And for path-based targets, path is generally a good
      // proxy for the target name. Since the bulk of the ad hoc recipes will
      // presumably be operating exclusively on path-based targets, let's do
      // it both ways.
      //
      {
        auto hash = [ns = names ()] (sha256& cs, const target& t) mutable
        {
          if (const path_target* pt = t.is_a<path_target> ())
            cs.append (pt->path ().string ());
          else
          {
            ns.clear ();
            t.as_name (ns);
            for (const name& n: ns)
              to_checksum (cs, n);
          }
        };

        sha256 tcs;
        for (const target* m (&t); m != nullptr; m = m->adhoc_member)
          hash (tcs, *m);

        if (dd.expect (tcs.string ()) != nullptr)
          l4 ([&]{trace << "target set change forcing update of " << t;});

        sha256 pcs;
        for (const target* pt: t.prerequisite_targets[a])
          if (pt != nullptr)
            hash (pcs, *pt);

        if (dd.expect (pcs.string ()) != nullptr)
          l4 ([&]{trace << "prerequisite set change forcing update of " << t;});
      }

      // Then the tools checksums.
      //
      // @@ TODO: obtain checksums of all the targets used as commands in
      //          the script.
      //
      //if (dd.expect (csum) != nullptr)
      //  l4 ([&]{trace << "compiler mismatch forcing update of " << t;});
    }

    // Update if depdb mismatch.
    //
    if (dd.writing () || dd.mtime > mt)
      update = true;

    dd.close ();

    // If nothing changed, then we are done.
    //
    if (!update)
      return *ps;

    if (verb == 1)
    {
      // @@ TODO (and below):
      //
      // - derive diag if absent (should probably do in match?)
      //
      // - we are printing target, not source (like in most other places)
      //
      // - printing of ad hoc target group (the {hxx cxx}{foo} idea)
      //
      // - if we are printing prerequisites, should we print all of them
      //   (including tools)?
      //

      text << (script.diag ? script.diag->c_str () : "adhoc") << ' ' << t;
    }

    if (!ctx.dry_run || verb >= 2)
    {
      const scope& bs (t.base_scope ());

      build::script::environment e (a, t, script.temp_dir);
      build::script::parser p (ctx);
      build::script::default_runner r;
      p.execute (*bs.root_scope (), bs, e, script, r);

      if (!ctx.dry_run)
        dd.check_mtime (tp);
    }

    t.mtime (system_clock::now ());
    return target_state::changed;
  }

  target_state adhoc_script_rule::
  default_action (action a, const target& t) const
  {
    tracer trace ("adhoc_rule::default_action");

    context& ctx (t.ctx);

    execute_prerequisites (a, t);

    if (verb == 1)
    {
      // @@ TODO: as above

      text << (script.diag ? script.diag->c_str () : "adhoc") << ' ' << t;
    }

    if (!ctx.dry_run || verb >= 2)
    {
      const scope& bs (t.base_scope ());

      build::script::environment e (a, t, script.temp_dir);
      build::script::parser p (ctx);
      build::script::default_runner r;
      p.execute (*bs.root_scope (), bs, e, script, r);
    }

    return target_state::changed;
  }

  // cxx_rule
  //
  bool cxx_rule::
  match (action, target&, const string&) const
  {
    return true;
  }

  // adhoc_cxx_rule
  //
  adhoc_cxx_rule::
  ~adhoc_cxx_rule ()
  {
    delete impl.load (memory_order_relaxed); // Serial execution.
  }

  void adhoc_cxx_rule::
  dump (ostream& os, string& ind) const
  {
    // @@ TODO: indentation is multi-line recipes is off (would need to insert
    //          indentation after every newline).
    //
    os << ind << string (braces, '{') << " c++" << endl
       << ind << code
       << ind << string (braces, '}');
  }

  // From module.cxx.
  //
  void
  create_module_context (context&, const location&);

  const target&
  update_in_module_context (context&, const scope&, names tgt,
                            const location&, const path& bf);

  pair<void*, void*>
  load_module_library (const path& lib, const string& sym, string& err);

  bool adhoc_cxx_rule::
  match (action a, target& t, const string& hint) const
  {
    tracer trace ("adhoc_cxx_rule::match");

    context& ctx (t.ctx);
    const scope& rs (t.root_scope ());

    // The plan is to reduce this to the build system module case as much as
    // possible. Specifically, we switch to the load phase, create a module-
    // like library with the recipe text as a rule implementation, then build
    // and load it.
    //
    // Since the recipe can be shared among multiple targets, several threads
    // can all be trying to do this in parallel.
    //
    // We use the relaxed memory order here because any change must go through
    // the serial load phase. In other words, all we need here is atomicity
    // with ordering/visibility provided by the phase mutex.
    //
    cxx_rule* impl (this->impl.load (memory_order_relaxed));

    while (impl == nullptr) // Breakout loop.
    {
      // Switch the phase to (serial) load and re-check.
      //
      phase_switch ps (ctx, run_phase::load);

      if ((impl = this->impl.load (memory_order_relaxed)) != nullptr)
        break;

      using create_function = cxx_rule* (const location&, target_state);
      using load_function = create_function* ();

      // The only way to guarantee that the name of our module matches its
      // implementation is to based the name on the implementation hash (plus
      // the language, in case we support other compiled implementations in
      // the future).
      //
      // Unfortunately, this means we will be creating a new project (and
      // leaving behind the old one as garbage) for every change to the
      // recipe. On the other hand, if the recipe is moved around unchanged,
      // we will reuse the same project. In fact, two different recipes (e.g.,
      // in different buildfiles) with the same text will share the project.
      //
      // The fact that we don't incorporate the recipe location into the hash
      // but include it in the source (in the form of the #line directive; see
      // below) has its own problems. If we do nothing extra here, then if a
      // "moved" but otherwise unchanged recipe is updated (for example,
      // because of changes in the build system core), then we may end up with
      // bogus location in the diagnostics.
      //
      // The straightforward solution would be to just update the location in
      // the source code if it has changed. This, however, will lead to
      // unnecessary and probably surprising recompilations since any line
      // count change before the recipe will trigger this update. One key
      // observation here is that we need accurate location information only
      // if we are going to recompile the recipe but the change to location
      // itself does not render the recipe out of date. So what we going to do
      // is factor the location information into its own small header and then
      // keep it up-to-date without changing its modification time.
      //
      // This works well if the project is not shared by multiple recipes.
      // However, if we have recipes in several buildfiles with identical
      // text, then the location information may end up yo-yo'ing depending on
      // which recipe got here first.
      //
      // There doesn't seem to be much we can do about it without incurring
      // other drawbacks/overheads. So the answer is for the user to use an ad
      // hoc rule with the common implementation instead of a bunch of
      // duplicate recipes.
      //
      string id;
      {
        sha256 cs;
        cs.append ("c++");
        cs.append (code);
        id = cs.abbreviated_string (12);
      }

      dir_path pd (rs.out_path () /
                   rs.root_extra->build_dir /
                   recipes_build_dir /= id);

      path bf (pd / std_buildfile_file);

      string sym ("load_" + id);

      // Check whether the file exists and its last line matches the specified
      // signature.
      //
      // Note: we use the last instead of the first line for extra protection
      // against incomplete writes.
      //
      auto check_sig = [] (const path& f, const string& s) -> bool
      {
        try
        {
          if (!file_exists (f))
            return false;

          ifdstream ifs (f);

          string l;
          while (ifs.peek () != ifdstream::traits_type::eof ())
            getline (ifs, l);

          return l == s;
        }
        catch (const io_error& e)
        {
          fail << "unable to read " << f << ": " << e << endf;
        }
        catch (const system_error& e)
        {
          fail << "unable to access " << f << ": " << e << endf;
        }
      };

      bool nested (ctx.module_context == &ctx);

      // Create the build context if necessary.
      //
      if (ctx.module_context == nullptr)
      {
        if (!ctx.module_context_storage)
          fail (loc) << "unable to update ad hoc recipe for target " << t <<
            info << "building of ad hoc recipes is disabled";

        create_module_context (ctx, loc);
      }

      // "Switch" to the module context.
      //
      context& ctx (*t.ctx.module_context);

      const uint16_t verbosity (3); // Project creation command verbosity.

      // Project and location signatures.
      //
      // Specifically, we update the project version when changing anything
      // which would make the already existing projects unusable.
      //
      const string& lf (!loc.file.path.empty ()
                        ? loc.file.path.string ()
                        : loc.file.name ? *loc.file.name : string ());

      const string psig ("# c++ 1");
      const string lsig ("// " + lf + ':' + to_string (loc.line));

      // Check whether we need to (re)create the project.
      //
      optional<bool> altn (false); // Standard naming scheme.
      bool create (!is_src_root (pd, altn));

      if (!create && (create = !check_sig (bf, psig)))
        rmdir_r (ctx, pd, false, verbosity); // Never dry-run.

      path of;
      ofdstream ofs;

      if (create)
      try
      {
        // Write ad hoc config.build that loads the ~build2 configuration.
        // This way the configuration will be always in sync with ~build2
        // and we can update the recipe manually (e.g., for debugging).
        //
        create_project (
          pd,
          dir_path (),                             /* amalgamation */
          {},                                      /* boot_modules */
          "cxx.std = latest",                      /* root_pre */
          {"cxx."},                                /* root_modules */
          "",                                      /* root_post */
          string ("config"),                       /* config_module */
          string ("config.config.load = ~build2"), /* config_file */
          false,                                   /* buildfile */
          "build2 core",                           /* who */
          verbosity);                              /* verbosity */


        // Write the rule source file.
        //
        of = path (pd / "rule.cxx");

        if (verb >= verbosity)
          text << (verb >= 2 ? "cat >" : "save ") << of;

        ofs.open (of);

        ofs << "#include \"location.hxx\""                              << '\n'
            << '\n';

        // Include every header that can plausibly be needed by a rule.
        //
        ofs << "#include <libbuild2/types.hxx>"                         << '\n'
            << "#include <libbuild2/forward.hxx>"                       << '\n'
            << "#include <libbuild2/utility.hxx>"                       << '\n'
            << '\n'
            << "#include <libbuild2/file.hxx>"                          << '\n'
            << "#include <libbuild2/rule.hxx>"                          << '\n'
            << "#include <libbuild2/depdb.hxx>"                         << '\n'
            << "#include <libbuild2/scope.hxx>"                         << '\n'
            << "#include <libbuild2/target.hxx>"                        << '\n'
            << "#include <libbuild2/context.hxx>"                       << '\n'
            << "#include <libbuild2/variable.hxx>"                      << '\n'
            << "#include <libbuild2/algorithm.hxx>"                     << '\n'
            << "#include <libbuild2/filesystem.hxx>"                    << '\n'
            << "#include <libbuild2/diagnostics.hxx>"                   << '\n'
            << '\n';

        // Normally the recipe code will have one level of indentation so
        // let's not indent the namespace level to match.
        //
        ofs << "namespace build2"                                       << '\n'
            << "{"                                                      << '\n'
            << '\n';

        // If we want the user to be able to supply a custom constuctor, then
        // we have to give the class a predictable name (i.e., we cannot use
        // id as part of its name) and put it into an unnamed namespace. One
        // clever idea is to call the class `constructor` but the name could
        // also be used for a custom destructor (still could work) or for name
        // qualification (would definitely look bizarre).
        //
        // In this light the most natural name is probable `rule`. The issue
        // is we already have this name in the build2 namespace (and its our
        // indirect base). In fact, any name that we choose could in the
        // future conflict with something in that namespace so maybe it makes
        // sense to bite the bullet and pick a name that is least likely to be
        // used by the user directly (can always use cxx_rule instead).
        //
        ofs << "namespace"                                              << '\n'
            << "{"                                                      << '\n'
            << "class rule: public cxx_rule"                            << '\n'
            << "{"                                                      << '\n'
            << "public:"                                                << '\n'
            << '\n';

        // Inherit base constructor. This way the user may provide their own
        // but don't have to.
        //
        ofs << "  using cxx_rule::cxx_rule;"                            << '\n'
            << '\n';

        // An extern "C" function cannot throw which can happen in case of a
        // user-defined constructor. So we need an extra level of indirection.
        // We incorporate id to make sure it doesn't conflict with anything
        // user-defined.
        //
        ofs << "  static cxx_rule*"                                     << '\n'
            << "  create_" << id << " (const location& l, target_state s)" << '\n'
            << "  {"                                                    << '\n'
            << "    return new rule (l, s);"                            << '\n'
            << "  }"                                                    << '\n'
            << '\n';

        // Use the #line directive to point diagnostics to the code in the
        // buildfile. Note that there is no easy way to restore things to
        // point back to the source file (other than another #line with a line
        // and a file). Seeing that we don't have much after, let's not bother
        // for now.
        //
        ofs << "#line RECIPE_LINE RECIPE_FILE"                          << '\n';

        // Note that the code always includes trailing newline.
        //
        ofs << code
            << "};"                                                     << '\n'
            << '\n';

        // Add an alias that we can use unambiguously in the load function.
        //
        ofs << "using rule_" << id << " = rule;"                        << '\n'
            << "}"                                                      << '\n'
            << '\n';

        // Entry point.
        //
        ofs << "extern \"C\""                                           << '\n'
            << "#ifdef _WIN32"                                          << '\n'
            << "__declspec(dllexport)"                                  << '\n'
            << "#endif"                                                 << '\n'
            << "cxx_rule* (*" << sym << " ()) (const location&, target_state)" << '\n'
            << "{"                                                      << '\n'
            << "  return &rule_" << id << "::create_" << id << ";"      << '\n'
            << "}"                                                      << '\n'
            << '\n';

        ofs << "}"                                                      << '\n';

        ofs.close ();


        // Write buildfile.
        //
        of = bf;

        if (verb >= verbosity)
          text << (verb >= 2 ? "cat >" : "save ") << of;

        ofs.open (of);

        ofs << "import imp_libs += build2%lib{build2}"                  << '\n'
            << "libs{" << id << "}: cxx{rule} hxx{location} $imp_libs"  << '\n'
            << '\n'
            << psig                                                     << '\n';

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << of << ": " << e;
      }

      // Update the library target in the module context.
      //
      const target* l (nullptr);
      do // Breakout loop.
      {
        // Load the project in the module context.
        //
        // Note that it's possible it has already been loaded (see above about
        // the id calculation).
        //
        scope& rs (load_project (ctx, pd, pd, false /* forwarded */));

        auto find_target = [&ctx, &rs, &pd, &id] ()
        {
          const target_type* tt (rs.find_target_type ("libs"));
          assert (tt != nullptr);

          const target* t (
            ctx.targets.find (*tt, pd, dir_path () /* out */, id));
          assert (t != nullptr);

          return t;
        };

        // If the project has already been loaded then, as an optimization,
        // check if the target has already been updated (this will make a
        // difference we if we have identical recipes in several buildfiles,
        // especially to the location update that comes next).
        //
        if (!source_once (rs, rs, bf))
        {
          l = find_target ();

          if (l->executed_state (perform_update_id) != target_state::unknown)
            break;
        }

        // Create/update the recipe location header.
        //
        // For update, preserve the file timestamp in order not to render the
        // recipe out of date.
        //
        of = path (pd / "location.hxx");
        if (!check_sig (of, lsig))
        try
        {
          entry_time et (file_time (of));

          if (verb >= verbosity)
            text << (verb >= 2 ? "cat >" : "save ") << of;

          ofs.open (of);

          // Recipe file and line for the #line directive above. Note that the
          // code starts from the next line thus +1. We also need to escape
          // backslashes (Windows paths).
          //
          ofs << "#define RECIPE_FILE \"" << sanitize_strlit (lf) << '"'<< '\n'
              << "#define RECIPE_LINE "   << loc.line + 1               << '\n'
              << '\n'
              << lsig                                                   << '\n';

          ofs.close ();

          if (et.modification != timestamp_nonexistent)
            file_time (of, et);
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << of << ": " << e;
        }
        catch (const system_error& e)
        {
          fail << "unable to get/set timestamp for " << of << ": " << e;
        }

        if (nested)
        {
          // This means there is a perform update action already in progress
          // in this context. So we are going to switch the phase and
          // perform direct match and update (similar how we do this for
          // generated headers).
          //
          // Note that since neither match nor execute are serial phases, it
          // means other targets in this context can be matched and executed
          // in paralellel with us.
          //
          if (l == nullptr)
            l = find_target ();

          phase_switch mp (ctx, run_phase::match);
          if (build2::match (perform_update_id, *l) != target_state::unchanged)
          {
            phase_switch ep (ctx, run_phase::execute);
            execute (a, *l);
          }
        }
        else
        {
          // Cutoff the existing diagnostics stack and push our own entry.
          //
          diag_frame::stack_guard diag_cutoff (nullptr);

          auto df = make_diag_frame (
            [this, &t] (const diag_record& dr)
            {
              dr << info (loc) << "while updating ad hoc recipe for target "
                 << t;
            });

          l = &update_in_module_context (
            ctx, rs, names {name (pd, "libs", id)},
            loc, bf);
        }
      } while (false);

      // Load the library.
      //
      const path& lib (l->as<file> ().path ());

      // Note again that it's possible the library has already been loaded
      // (see above about the id calculation).
      //
      string err;
      pair<void*, void*> hs (load_module_library (lib, sym, err));

      // These normally shouldn't happen unless something is seriously broken.
      //
      if (hs.first == nullptr)
        fail (loc) << "unable to load recipe library " << lib << ": " << err;

      if (hs.second == nullptr)
        fail (loc) << "unable to lookup " << sym << " in recipe library "
                   << lib << ": " << err;

      {
        auto df = make_diag_frame (
          [this](const diag_record& dr)
          {
            if (verb != 0)
              dr << info (loc) << "while initializing ad hoc recipe";
          });

        load_function* lf (function_cast<load_function*> (hs.second));
        create_function* cf (lf ());

        impl = cf (loc, l->executed_state (perform_update_id));
        this->impl.store (impl, memory_order_relaxed); // Still in load phase.
      }
    }

    return impl->match (a, t, hint);
  }

  recipe adhoc_cxx_rule::
  apply (action a, target& t) const
  {
    return impl.load (memory_order_relaxed)->apply (a, t);
  }
}
