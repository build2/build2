// file      : libbuild2/adhoc-rule-buildscript.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/adhoc-rule-buildscript.hxx>

#include <sstream>

#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/dyndep.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>  // path_perms(), auto_rmfile
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/make-parser.hxx>

#include <libbuild2/parser.hxx> // attributes

#include <libbuild2/build/script/parser.hxx>
#include <libbuild2/build/script/runner.hxx>

using namespace std;

namespace build2
{
  static inline void
  hash_script_vars (sha256& cs,
                    const build::script::script& s,
                    const target& t,
                    names& storage)
  {
    context& ctx (t.ctx);

    for (const string& n: s.vars)
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
  }

  // How should we hash target and prerequisite sets ($> and $<)? We could
  // hash them as target names (i.e., the same as the $>/< content) or as
  // paths (only for path-based targets). While names feel more general, they
  // are also more expensive to compute. And for path-based targets, path is
  // generally a good proxy for the target name. Since the bulk of the ad hoc
  // recipes will presumably be operating exclusively on path-based targets,
  // let's do it both ways.
  //
  static inline void
  hash_target (sha256& cs, const target& t, names& storage)
  {
    if (const path_target* pt = t.is_a<path_target> ())
      cs.append (pt->path ().string ());
    else
    {
      storage.clear ();
      t.as_name (storage);
      for (const name& n: storage)
        to_checksum (cs, n);
    }
  };

  // The script can reference a program in one of four ways:
  //
  // 1. As an (imported) target (e.g., $cli)
  //
  // 2. As a process_path_ex (e.g., $cxx.path).
  //
  // 3. As a builtin (e.g., sed)
  //
  // 4. As a program path/name.
  //
  // When it comes to change tracking, there is nothing we can do for (4) (the
  // user can track its environment manually with depdb-env) and there is
  // nothing to do for (3) (assuming builtin semantics is stable/backwards-
  // compatible). The (2) case is handled automatically by hashing all the
  // variable values referenced by the script (see below), which in case of
  // process_path_ex includes the checksums (both executable and environment),
  // if available.
  //
  // This leaves the (1) case, which itself splits into two sub-cases: the
  // target comes with the dependency information (e.g., imported from a
  // project via an export stub) or it does not (e.g., imported as installed).
  // We don't need to do anything extra for the first sub-case since the
  // target's state/mtime can be relied upon like any other prerequisite.
  // Which cannot be said about the second sub-case, where we reply on
  // checksum that may be included as part of the target metadata.
  //
  // So what we are going to do is hash checksum metadata of every executable
  // prerequisite target that has it (we do it here in order to include ad hoc
  // prerequisites, which feels like the right thing to do; the user may mark
  // tools as ad hoc in order to omit them from $<).
  //
  static inline void
  hash_prerequisite_target (sha256& cs, sha256& exe_cs, sha256& env_cs,
                            const target& pt,
                            names& storage)
  {
    hash_target (cs, pt, storage);

    if (const exe* et = pt.is_a<exe> ())
    {
      if (const string* c = et->lookup_metadata<string> ("checksum"))
      {
        exe_cs.append (*c);
      }

      if (const strings* e = et->lookup_metadata<strings> ("environment"))
      {
        hash_environment (env_cs, *e);
      }
    }
  }

  bool adhoc_buildscript_rule::
  recipe_text (const scope& s,
               const target_type& tt,
               string&& t,
               attributes& as)
  {
    // Handle and erase recipe-specific attributes.
    //
    optional<string> diag;
    for (auto i (as.begin ()); i != as.end (); )
    {
      attribute& a (*i);
      const string& n (a.name);

      if (n == "diag")
      try
      {
        diag = convert<string> (move (a.value));
      }
      catch (const invalid_argument& e)
      {
        fail (as.loc) << "invalid " << n << " attribute value: " << e;
      }
      else
      {
        ++i;
        continue;
      }

      i = as.erase (i);
    }

    checksum = sha256 (t).string ();
    ttype = &tt;

    istringstream is (move (t));
    build::script::parser p (s.ctx);

    script = p.pre_parse (s, tt, actions,
                          is, loc.file, loc.line + 1,
                          move (diag), as.loc);

    return false;
  }

  void adhoc_buildscript_rule::
  dump_attributes (ostream& os) const
  {
    // For now we dump it as an attribute whether it was specified or derived
    // from the script. Maybe that's ok (we use this in tests)?
    //
    if (script.diag_name)
    {
      os << " [";
      os << "diag=";
      to_stream (os, name (*script.diag_name), quote_mode::normal, '@');
      os << ']';
    }
  }

  void adhoc_buildscript_rule::
  dump_text (ostream& os, string& ind) const
  {
    os << ind << string (braces, '{') << endl;
    ind += "  ";

    if (script.depdb_clear)
      os << ind << "depdb clear" << endl;

    script::dump (os, ind, script.depdb_preamble);

    if (script.diag_line)
    {
      os << ind; script::dump (os, *script.diag_line, true /* newline */);
    }

    script::dump (os, ind, script.body);
    ind.resize (ind.size () - 2);
    os << ind << string (braces, '}');
  }

  bool adhoc_buildscript_rule::
  reverse_fallback (action a, const target_type& tt) const
  {
    // We can provide clean for a file target if we are providing update.
    //
    return a == perform_clean_id && tt.is_a<file> () &&
      find (actions.begin (), actions.end (),
            perform_update_id) != actions.end ();
  }

  struct adhoc_buildscript_rule::match_data
  {
    match_data (action a, const target& t, bool temp_dir)
        : env (a, t, temp_dir) {}

    build::script::environment env;
    build::script::default_runner run;

    path dd;

    const scope* bs;
    timestamp mt;
    bool deferred_failure;
  };

  struct adhoc_buildscript_rule::match_data_byproduct
  {
    match_data_byproduct (action a, const target& t, bool temp_dir)
        : env (a, t, temp_dir) {}

    build::script::environment env;
    build::script::default_runner run;

    build::script::parser::dyndep_byproduct byp;

    depdb::reopen_state dd;
    size_t skip_count = 0;
    size_t pts_n; // Number of static prerequisites in prerequisite_targets.

    const scope* bs;
    timestamp mt;
  };

  bool adhoc_buildscript_rule::
  match (action a, target& t, const string& h, match_extra& me) const
  {
    // We pre-parsed the script with the assumption it will be used on a
    // non/file-based target. Note that this should not be possible with
    // patterns.
    //
    if (pattern == nullptr)
    {
      if ((t.is_a<file> () != nullptr) != ttype->is_a<file> ())
      {
        fail (loc) << "incompatible target types used with shared recipe" <<
          info << "all targets must be file-based or non-file-based";
      }
    }

    return adhoc_rule::match (a, t, h, me);
  }

  recipe adhoc_buildscript_rule::
  apply (action a, target& t, match_extra& me) const
  {
    return apply (a, t, me, nullopt);
  }

  recipe adhoc_buildscript_rule::
  apply (action a,
         target& xt,
         match_extra& me,
         const optional<timestamp>& d) const
  {
    tracer trace ("adhoc_buildscript_rule::apply");

    // We don't support deadlines for any of these cases (see below).
    //
    if (d && (a.outer ()  ||
              me.fallback ||
              (a == perform_update_id && xt.is_a<file> ())))
      return empty_recipe;

    // If this is an outer operation (e.g., update-for-test), then delegate to
    // the inner.
    //
    if (a.outer ())
    {
      match_inner (a, xt);
      return execute_inner;
    }

    context& ctx (xt.ctx);
    const scope& bs (xt.base_scope ());

    // Inject pattern's ad hoc group members, if any.
    //
    if (pattern != nullptr)
      pattern->apply_adhoc_members (a, xt, bs, me);

    // Derive file names for the target and its ad hoc group members, if any.
    //
    if (a == perform_update_id || a == perform_clean_id)
    {
      for (target* m (&xt); m != nullptr; m = m->adhoc_member)
      {
        if (auto* p = m->is_a<path_target> ())
          p->derive_path ();
      }
    }

    // Inject dependency on the output directory.
    //
    // We do it always instead of only if one of the targets is path-based in
    // case the recipe creates temporary files or some such.
    //
    const fsdir* dir (inject_fsdir (a, xt));

    // Match prerequisites.
    //
    // This is essentially match_prerequisite_members() but with support
    // for update=unmatch|match.
    //
    auto& pts (xt.prerequisite_targets[a]);
    {
      // Re-create the clean semantics as in match_prerequisite_members().
      //
      bool clean (a.operation () == clean_id && !xt.is_a<alias> ());

      // Add target's prerequisites.
      //
      for (prerequisite_member p: group_prerequisite_members (a, xt))
      {
        // Note that we have to recognize update=unmatch|match for *(update),
        // not just perform(update). But only actually do anything about it
        // for perform(update).
        //
        lookup l; // The `update` variable value, if any.
        include_type pi (
          include (a, xt, p, a.operation () == update_id ? &l : nullptr));

        // Use bit 2 of prerequisite_target::include to signal update during
        // match and bit 3 -- unmatch.
        //
        uintptr_t mask (0);
        if (l)
        {
          const string& v (cast<string> (l));

          if (v == "match")
          {
            if (a == perform_update_id)
              mask = 2;
          }
          else if (v == "unmatch")
          {
            if (a == perform_update_id)
              mask = 4;
          }
          else if (v != "false" && v != "true" && v != "execute")
          {
            fail << "unrecognized update variable value '" << v
                 << "' specified for prerequisite " << p.prerequisite;
          }
        }

        // Skip excluded.
        //
        if (!pi)
          continue;

        const target& pt (p.search (xt));

        if (clean && !pt.in (*bs.root_scope ()))
          continue;

        prerequisite_target pto (&pt, pi);

        if (mask != 0)
          pto.include |= mask;

        pts.push_back (move (pto));
      }

      // Inject pattern's prerequisites, if any.
      //
      if (pattern != nullptr)
        pattern->apply_prerequisites (a, xt, bs, me);

      // Start asynchronous matching of prerequisites. Wait with unlocked
      // phase to allow phase switching.
      //
      wait_guard wg (ctx, ctx.count_busy (), xt[a].task_count, true);

      for (const prerequisite_target& pt: pts)
      {
        if (pt.target == dir)
          continue;

        match_async (a, *pt.target, ctx.count_busy (), xt[a].task_count);
      }

      wg.wait ();

      // Finish matching all the targets that we have started.
      //
      for (prerequisite_target& pt: pts)
      {
        if (pt.target == dir)
          continue;

        // Handle update=unmatch.
        //
        unmatch um ((pt.include & 4) != 0 ? unmatch::safe : unmatch::none);

        pair<bool, target_state> mr (match_complete (a, *pt.target, um));

        if (um != unmatch::none)
        {
          l6 ([&]{trace << "unmatch " << *pt.target << ": " << mr.first;});

          // If we managed to unmatch, blank it out so that it's not executed,
          // etc. Otherwise, convert it to ad hoc (we also automatically avoid
          // hashing it, updating it during match in exec_depdb_dyndep(), and
          // making us out of date in execute_update_prerequisites()).
          //
          // The hashing part is tricky: by not hashing it we won't detect the
          // case where it was removed as a prerequisite altogether. The
          // thinking is that it was added with update=unmatch to extract some
          // information (e.g., poptions from a library) and those will be
          // change-tracked.
          //
          if (mr.first)
            pt.target = nullptr;
          else
            pt.include |= 1;
        }
      }
    }

    // See if we are providing the standard clean as a fallback.
    //
    if (me.fallback)
      return &perform_clean_file;

    // If we have any update during match prerequisites, now is the time to
    // update them.
    //
    // Note that we ignore the result and whether it renders us out of date,
    // leaving it to the common execute logic in perform_update_*().
    //
    // Note also that update_during_match_prerequisites() spoils
    // prerequisite_target::data.
    //
    if (a == perform_update_id)
      update_during_match_prerequisites (trace, a, xt, 2 /* mask */);

    // See if this is not update or not on a file-based target.
    //
    if (a != perform_update_id || !xt.is_a<file> ())
    {
      return [d, this] (action a, const target& t)
      {
        return default_action (a, t, d);
      };
    }

    // See if this is the simple case with only static dependencies.
    //
    if (!script.depdb_dyndep)
    {
      return [this] (action a, const target& t)
      {
        return perform_update_file (a, t);
      };
    }

    // This is a perform update on a file target with extraction of dynamic
    // dependency information either in the depdb preamble (depdb-dyndep
    // without --byproduct) or as a byproduct of the recipe body execution
    // (depdb-dyndep with --byproduct).
    //
    // For the former case, we may need to add additional prerequisites (or
    // even target group members). We also have to save any such additional
    // prerequisites in depdb so that we can check if any of them have changed
    // on subsequent updates. So all this means that we have to take care of
    // depdb here in apply() instead of perform_*() like we normally do. We
    // also do things in slightly different order due to the restrictions
    // impose by the match phase.
    //
    // The latter case (depdb-dyndep --byproduct) is sort of a combination
    // of the normal dyndep and the static case: we check the depdb during
    // match but save after executing the recipe.
    //
    // Note that the C/C++ header dependency extraction is the canonical
    // example and all this logic is based on the prior work in the cc module
    // where you can often find more detailed rationale for some of the steps
    // performed (like the fsdir update below).
    //
    file& t (xt.as<file> ());
    const path& tp (t.path ());

    if (dir != nullptr)
      fsdir_rule::perform_update_direct (a, t);

    // Because the depdb preamble can access $<, we have to blank out all the
    // ad hoc prerequisites. Since we will still need them later, we "move"
    // them to the auxiliary data member in prerequisite_target (see
    // execute_update_prerequisites() for details).
    //
    for (prerequisite_target& p: pts)
    {
      // Note that fsdir{} injected above is adhoc.
      //
      if (p.target != nullptr && p.adhoc ())
      {
        p.data = reinterpret_cast<uintptr_t> (p.target);
        p.target = nullptr;
      }
    }

    depdb dd (tp + ".d");

    // NOTE: see the "static dependencies" version (with comments) below.
    //
    if (dd.expect ("<ad hoc buildscript recipe> 1") != nullptr)
      l4 ([&]{trace << "rule mismatch forcing update of " << t;});

    if (dd.expect (checksum) != nullptr)
      l4 ([&]{trace << "recipe text change forcing update of " << t;});

    if (!script.depdb_clear)
    {
      names storage;

      sha256 prq_cs, exe_cs, env_cs;

      for (const prerequisite_target& p: pts)
      {
        if (const target* pt =
            (p.target != nullptr ? p.target :
             p.adhoc ()          ? reinterpret_cast<target*> (p.data) :
             nullptr))
        {
          if ((p.include & 4) != 0) // Skip update=unmatch.
            continue;

          hash_prerequisite_target (prq_cs, exe_cs, env_cs, *pt, storage);
        }
      }

      {
        sha256 cs;
        hash_script_vars (cs, script, t, storage);

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "recipe variable change forcing update of " << t;});
      }

      {
        sha256 tcs;
        for (const target* m (&t); m != nullptr; m = m->adhoc_member)
          hash_target (tcs, *m, storage);

        if (dd.expect (tcs.string ()) != nullptr)
          l4 ([&]{trace << "target set change forcing update of " << t;});

        if (dd.expect (prq_cs.string ()) != nullptr)
          l4 ([&]{trace << "prerequisite set change forcing update of " << t;});
      }

      {
        if (dd.expect (exe_cs.string ()) != nullptr)
          l4 ([&]{trace << "program checksum change forcing update of " << t;});

        if (dd.expect (env_cs.string ()) != nullptr)
          l4 ([&]{trace << "environment change forcing update of " << t;});
      }
    }

    unique_ptr<match_data> md;
    unique_ptr<match_data_byproduct> mdb;

    if (script.depdb_dyndep_byproduct)
    {
      mdb.reset (new match_data_byproduct (
                   a, t, script.depdb_preamble_temp_dir));
    }
    else
      md.reset (new match_data (a, t, script.depdb_preamble_temp_dir));


    build::script::environment& env (mdb != nullptr ? mdb->env : md->env);
    build::script::default_runner& run (mdb != nullptr ? mdb->run : md->run);

    run.enter (env, script.start_loc);

    // Run the first half of the preamble (before depdb-dyndep).
    //
    {
      build::script::parser p (ctx);
      p.execute_depdb_preamble (a, bs, t, env, script, run, dd);
    }

    // Determine if we need to do an update based on the above checks.
    //
    bool update;
    timestamp mt;

    if (dd.writing ())
      update = true;
    else
    {
      if ((mt = t.mtime ()) == timestamp_unknown)
        t.mtime (mt = mtime (tp)); // Cache.

      update = dd.mtime > mt;
    }

    if (update)
      mt = timestamp_nonexistent;

    if (script.depdb_dyndep_byproduct)
    {
      // If we have the dynamic dependency information as byproduct of the
      // recipe body, then do the first part: verify the entries in depdb
      // unless we are already updating. Essentially, this is the `if(cache)`
      // equivalent of the restart loop in exec_depdb_dyndep().

      using dyndep = dyndep_rule;

      // Update our prerequisite targets and extract the depdb-dyndep
      // command's information (we may also execute some variable
      // assignments).
      //
      // Do we really need to update our prerequisite targets in this case?
      // While it may seem like we should be able to avoid it by triggering
      // update on encountering any non-existent files in depbd, we may
      // actually incorrectly "validate" some number of depdb entires while
      // having an out-of-date main source file. We could probably avoid the
      // update if we are already updating.
      //
      {
        build::script::parser p (ctx);
        mdb->byp = p.execute_depdb_preamble_dyndep_byproduct (
          a, bs, t,
          env, script, run,
          dd, update, mt);
      }

      mdb->pts_n = pts.size ();

      if (!update)
      {
        const auto& byp (mdb->byp);
        const char* what (byp.what.c_str ());
        const location& ll (byp.location);

        function<dyndep::map_extension_func> map_ext (
          [] (const scope& bs, const string& n, const string& e)
          {
            // NOTE: another version in exec_depdb_dyndep().

            return dyndep::map_extension (bs, n, e, nullptr);
          });

        // Similar to exec_depdb_dyndep()::add() but only for cache=true and
        // without support for generated files.
        //
        // Note that we have to update each file for the same reason as the
        // main source file -- if any of them changed, then we must assume the
        // subsequent entries are invalid.
        //
        size_t& skip_count (mdb->skip_count);

        auto add = [&trace, what,
                    a, &bs, &t,
                    &byp, &map_ext,
                    &skip_count, mt] (path fp) -> optional<bool>
        {
          if (const build2::file* ft = dyndep::enter_file (
                trace, what,
                a, bs, t,
                fp, true /* cache */, true /* normalized */,
                map_ext, *byp.default_type).first)
          {
            // Note: mark the injected prerequisite target as updated (see
            // execute_update_prerequisites() for details).
            //
            if (optional<bool> u = dyndep::inject_existing_file (
                  trace, what,
                  a, t,
                  *ft, mt,
                  false /* fail */,
                  false /* adhoc */,
                  1     /* data */))
            {
              skip_count++;
              return *u;
            }
          }

          return nullopt;
        };

        auto df = make_diag_frame (
          [&ll, &t] (const diag_record& dr)
          {
            if (verb != 0)
              dr << info (ll) << "while extracting dynamic dependencies for "
                 << t;
          });

        while (!update)
        {
          // We should always end with a blank line.
          //
          string* l (dd.read ());

          // If the line is invalid, run the compiler.
          //
          if (l == nullptr)
          {
            update = true;
            break;
          }

          if (l->empty ()) // Done, nothing changed.
            break;

          if (optional<bool> r = add (path (move (*l))))
          {
            if (*r)
              update = true;
          }
          else
          {
            // Invalidate this line and trigger update.
            //
            dd.write ();
            update = true;
          }

          if (update)
            l6 ([&]{trace << "restarting (cache)";});
        }
      }

      // Note that in case of dry run we will have an incomplete (but valid)
      // database which will be updated on the next non-dry run.
      //
      if (!update || ctx.dry_run)
        dd.close (false /* mtime_check */);
      else
        mdb->dd = dd.close_to_reopen ();

      // Pass on base scope and update/mtime.
      //
      mdb->bs = &bs;
      mdb->mt = update ? timestamp_nonexistent : mt;

      // @@ TMP: re-enable once recipe becomes move_only_function.
      //
#if 0
      return [this, md = move (mdb)] (action a, const target& t) mutable
      {
        auto r (perform_update_file_dyndep_byproduct (a, t, *md));
        md.reset (); // @@ TMP: is this really necessary (+mutable)?
        return r;
      };
#else
      t.data (move (mdb));
      return recipe ([this] (action a, const target& t) mutable
      {
        auto md (move (t.data<unique_ptr<match_data_byproduct>> ()));
        return perform_update_file_dyndep_byproduct (a, t, *md);
      });
#endif
    }
    else
    {
      // Run the second half of the preamble (depdb-dyndep commands) to update
      // our prerequisite targets and extract dynamic dependencies.
      //
      // Note that this should be the last update to depdb (the invalidation
      // order semantics).
      //
      bool deferred_failure (false);
      {
        build::script::parser p (ctx);
        p.execute_depdb_preamble_dyndep (a, bs, t,
                                         env, script, run,
                                         dd,
                                         update,
                                         mt,
                                         deferred_failure);
      }

      if (update && dd.reading () && !ctx.dry_run)
        dd.touch = timestamp_unknown;

      dd.close (false /* mtime_check */);
      md->dd = move (dd.path);

      // Pass on base scope and update/mtime.
      //
      md->bs = &bs;
      md->mt = update ? timestamp_nonexistent : mt;
      md->deferred_failure = deferred_failure;

      // @@ TMP: re-enable once recipe becomes move_only_function.
      //
#if 0
      return [this, md = move (md)] (action a, const target& t) mutable
      {
        auto r (perform_update_file_dyndep (a, t, *md));
        md.reset (); // @@ TMP: is this really necessary (+mutable)?
        return r;
      };
#else
      t.data (move (md));
      return recipe ([this] (action a, const target& t) mutable
      {
        auto md (move (t.data<unique_ptr<match_data>> ()));
        return perform_update_file_dyndep (a, t, *md);
      });
#endif
    }
  }

  target_state adhoc_buildscript_rule::
  perform_update_file_dyndep_byproduct (action a,
                                        const target& xt,
                                        match_data_byproduct& md) const
  {
    // Note: using shared function name among the three variants.
    //
    tracer trace ("adhoc_buildscript_rule::perform_update_file");

    context& ctx (xt.ctx);

    const file& t (xt.as<file> ());

    // Note that even if we've updated all our prerequisites in apply(), we
    // still need to execute them here to keep the dependency counts straight.
    //
    optional<target_state> ps (execute_update_prerequisites (a, t, md.mt));

    if (!ps)
      md.mt = timestamp_nonexistent; // Update.

    build::script::environment& env (md.env);
    build::script::default_runner& run (md.run);

    if (md.mt != timestamp_nonexistent)
    {
      run.leave (env, script.end_loc);
      return *ps;
    }

    const scope& bs (*md.bs);

    // Sequence start time for mtime checks below.
    //
    timestamp start (!ctx.dry_run && depdb::mtime_check ()
                     ? system_clock::now ()
                     : timestamp_unknown);

    if (!ctx.dry_run || verb != 0)
    {
      execute_update_file (bs, a, t, env, run);
    }

    // Extract the dynamic dependency information as byproduct of the recipe
    // body. Essentially, this is the `if(!cache)` equivalent of the restart
    // loop in exec_depdb_dyndep().
    //
    if (!ctx.dry_run)
    {
      using dyndep = dyndep_rule;
      using dyndep_format = build::script::parser::dyndep_format;

      depdb dd (move (md.dd));

      const auto& byp (md.byp);
      const location& ll (byp.location);
      const char* what (byp.what.c_str ());
      const path& file (byp.file);

      env.clean ({build2::script::cleanup_type::always, file},
                 true /* implicit */);

      function<dyndep::map_extension_func> map_ext (
        [] (const scope& bs, const string& n, const string& e)
        {
          // NOTE: another version in exec_depdb_dyndep() and above.

          return dyndep::map_extension (bs, n, e, nullptr);
        });

      // Analogous to exec_depdb_dyndep()::add() but only for cache=false.
      // The semantics is quite different, however: instead of updating the
      // dynamic prerequisites we verify they are not generated.
      //
      // Note that fp is expected to be absolute.
      //
      size_t skip (md.skip_count);
      const auto& pts (t.prerequisite_targets[a]);

      auto add = [&trace, what,
                  a, &bs, &t, &pts, pts_n = md.pts_n,
                  &byp, &map_ext, &dd, &skip] (path fp)
      {
        normalize_external (fp, what);

        if (const build2::file* ft = dyndep::find_file (
              trace, what,
              a, bs, t,
              fp, false /* cache */, true /* normalized */,
              map_ext, *byp.default_type).first)
        {
          // Skip if this is one of the static prerequisites provided it was
          // updated.
          //
          for (size_t i (0); i != pts_n; ++i)
          {
            const prerequisite_target& p (pts[i]);

            if (const target* pt =
                (p.target != nullptr ? p.target :
                 p.adhoc ()          ? reinterpret_cast<target*> (p.data) :
                 nullptr))
            {
              if (ft == pt && (p.adhoc () || p.data == 1))
                return;
            }
          }

          // Skip if this is one of the targets.
          //
          if (byp.drop_cycles)
          {
            for (const target* m (&t); m != nullptr; m = m->adhoc_member)
            {
              if (ft == m)
                return;
            }
          }

          // Skip until where we left off.
          //
          if (skip != 0)
          {
            --skip;
            return;
          }

          // Verify it has noop recipe.
          //
          // @@ Currently we will issue an imprecise diagnostics if this is
          //    a static prerequisite that was not updated (see above).
          //
          dyndep::verify_existing_file (trace, what, a, t, *ft);
        }

        dd.write (fp);
      };

      auto df = make_diag_frame (
        [&ll, &t] (const diag_record& dr)
        {
          if (verb != 0)
            dr << info (ll) << "while extracting dynamic dependencies for "
               << t;
        });

      ifdstream is (ifdstream::badbit);
      try
      {
        is.open (file);
      }
      catch (const io_error& e)
      {
        fail (ll) << "unable to open file " << file << ": " << e;
      }

      location il (file, 1);

      // The way we parse things is format-specific.
      //
      // Note: similar code in exec_depdb_dyndep(). Except here we just add
      // the paths to depdb without entering them as targets.
      //
      switch (md.byp.format)
      {
      case dyndep_format::make:
        {
          using make_state = make_parser;
          using make_type = make_parser::type;

          make_parser make;

          for (string l;; ++il.line) // Reuse the buffer.
          {
            if (eof (getline (is, l)))
            {
              if (make.state != make_state::end)
                fail (il) << "incomplete make dependency declaration";

              break;
            }

            size_t pos (0);
            do
            {
              // Note that we don't really need a diag frame that prints the
              // line being parsed since we are always parsing the file.
              //
              pair<make_type, path> r (make.next (l, pos, il));

              if (r.second.empty ())
                continue;

              // @@ TODO: what should we do about targets?
              //
              if (r.first == make_type::target)
                continue;

              path& f (r.second);

              if (f.relative ())
              {
                if (!byp.cwd)
                  fail (il) << "relative path '" << f << "' in make dependency"
                            << " declaration" <<
                    info << "consider using --cwd to specify relative path "
                            << "base";

                f = *byp.cwd / f;
              }

              add (move (f));
            }
            while (pos != l.size ());

            if (make.state == make_state::end)
              break;
          }

          break;
        }
      }

      // Add the terminating blank line.
      //
      dd.expect ("");
      dd.close ();

      md.dd.path = move (dd.path); // For mtime check below.
    }

    run.leave (env, script.end_loc);

    timestamp now (system_clock::now ());

    if (!ctx.dry_run)
      depdb::check_mtime (start, md.dd.path, t.path (), now);

    t.mtime (now);
    return target_state::changed;
  }

  target_state adhoc_buildscript_rule::
  perform_update_file_dyndep (action a, const target& xt, match_data& md) const
  {
    tracer trace ("adhoc_buildscript_rule::perform_update_file");

    context& ctx (xt.ctx);

    const file& t (xt.as<file> ());

    // Note that even if we've updated all our prerequisites in apply(), we
    // still need to execute them here to keep the dependency counts straight.
    //
    optional<target_state> ps (execute_update_prerequisites (a, t, md.mt));

    if (!ps)
      md.mt = timestamp_nonexistent; // Update.

    build::script::environment& env (md.env);
    build::script::default_runner& run (md.run);

    // Force update in case of a deferred failure even if nothing changed.
    //
    if (md.mt != timestamp_nonexistent && !md.deferred_failure)
    {
      run.leave (env, script.end_loc);
      return *ps;
    }

    // Sequence start time for mtime checks below.
    //
    timestamp start (!ctx.dry_run && depdb::mtime_check ()
                     ? system_clock::now ()
                     : timestamp_unknown);

    if (!ctx.dry_run || verb != 0)
    {
      execute_update_file (*md.bs, a, t, env, run, md.deferred_failure);
    }

    run.leave (env, script.end_loc);

    timestamp now (system_clock::now ());

    if (!ctx.dry_run)
      depdb::check_mtime (start, md.dd, t.path (), now);

    t.mtime (now);
    return target_state::changed;
  }

  target_state adhoc_buildscript_rule::
  perform_update_file (action a, const target& xt) const
  {
    tracer trace ("adhoc_buildscript_rule::perform_update_file");

    context& ctx (xt.ctx);

    const file& t (xt.as<file> ());
    const path& tp (t.path ());

    // Update prerequisites and determine if any of them render this target
    // out-of-date.
    //
    timestamp mt (t.load_mtime ());

    // This is essentially ps=execute_prerequisites(a, t, mt) which we
    // cannot use because we need to see ad hoc prerequisites.
    //
    optional<target_state> ps (execute_update_prerequisites (a, t, mt));

    // Calculate prerequisite checksums (that need to include ad hoc
    // prerequisites) unless the script tracks changes itself.
    //
    names storage;
    sha256 prq_cs, exe_cs, env_cs;

    if (!script.depdb_clear)
    {
      for (const prerequisite_target& p: t.prerequisite_targets[a])
      {
        if (const target* pt =
            (p.target != nullptr ? p.target :
             p.adhoc ()          ? reinterpret_cast<target*> (p.data)
             : nullptr))
        {
          if ((p.include & 4) != 0) // Skip update=unmatch.
            continue;

          hash_prerequisite_target (prq_cs, exe_cs, env_cs, *pt, storage);
        }
      }
    }

    bool update (!ps);

    // We use depdb to track changes to the script itself, input/output file
    // names, tools, etc.
    //
    // NOTE: see the "dynamic dependencies" version above.
    //
    depdb dd (tp + ".d");

    // First should come the rule name/version.
    //
    if (dd.expect ("<ad hoc buildscript recipe> 1") != nullptr)
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

    // Track the variables, targets, and prerequisites changes, unless the
    // script tracks the dependency changes itself.
    //
    if (!script.depdb_clear)
    {
      // For each variable hash its name, undefined/null/non-null indicator,
      // and the value if non-null.
      //
      // Note that this excludes the special $< and $> variables which we
      // handle below.
      //
      // @@ TODO: maybe detect and decompose process_path_ex in order to
      //    properly attribute checksum and environment changes?
      //
      {
        sha256 cs;
        hash_script_vars (cs, script, t, storage);

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "recipe variable change forcing update of " << t;});
      }

      // Target and prerequisite sets ($> and $<).
      //
      {
        sha256 tcs;
        for (const target* m (&t); m != nullptr; m = m->adhoc_member)
          hash_target (tcs, *m, storage);

        if (dd.expect (tcs.string ()) != nullptr)
          l4 ([&]{trace << "target set change forcing update of " << t;});

        if (dd.expect (prq_cs.string ()) != nullptr)
          l4 ([&]{trace << "prerequisite set change forcing update of " << t;});
      }

      // Finally the programs and environment checksums.
      //
      {
        if (dd.expect (exe_cs.string ()) != nullptr)
          l4 ([&]{trace << "program checksum change forcing update of " << t;});

        if (dd.expect (env_cs.string ()) != nullptr)
          l4 ([&]{trace << "environment change forcing update of " << t;});
      }
    }

    const scope* bs (nullptr);

    // Execute the custom dependency change tracking commands, if present.
    //
    // Note that we share the environment between the execute_depdb_preamble()
    // and execute_body() calls, which is not merely an optimization since
    // variables set in the preamble must be available in the body.
    //
    // Creating the environment instance is not cheap so optimize for the
    // common case where we don't have the depdb preamble and nothing to
    // update.
    //
    bool depdb_preamble (!script.depdb_preamble.empty ());

    if (!depdb_preamble)
    {
      if (dd.writing () || dd.mtime > mt)
        update = true;

      if (!update)
      {
        dd.close ();
        return *ps;
      }
    }

    build::script::environment env (a, t, false /* temp_dir */);
    build::script::default_runner run;

    if (depdb_preamble)
    {
      bs = &t.base_scope ();

      if (script.depdb_preamble_temp_dir)
        env.set_temp_dir_variable ();

      build::script::parser p (ctx);

      run.enter (env, script.start_loc);
      p.execute_depdb_preamble (a, *bs, t, env, script, run, dd);
    }

    // Update if depdb mismatch.
    //
    if (dd.writing () || dd.mtime > mt)
      update = true;

    dd.close ();

    // If nothing changed, then we are done.
    //
    if (!update)
    {
      // Note that if we execute the depdb preamble but not the script body,
      // we need to call the runner's leave() function explicitly (here and
      // below).
      //
      if (depdb_preamble)
        run.leave (env, script.end_loc);

      return *ps;
    }

    bool r (false);
    if (!ctx.dry_run || verb != 0)
    {
      // Prepare to execute the script diag line and/or body.
      //
      if (bs == nullptr)
        bs = &t.base_scope ();

      if ((r = execute_update_file (*bs, a, t, env, run)))
      {
        if (!ctx.dry_run)
          dd.check_mtime (tp);
      }
    }

    if (r || depdb_preamble)
      run.leave (env, script.end_loc);

    t.mtime (system_clock::now ());
    return target_state::changed;
  }

  // Update prerequisite targets.
  //
  // Each (non-NULL) prerequisite target should be in one of the following
  // states:
  //
  // target  adhoc  data
  // --------------------
  // !NULL   false  0      - normal prerequisite to be updated
  // !NULL   false  1      - normal prerequisite already updated
  // !NULL   true   0      - ad hoc prerequisite to be updated and blanked
  //  NULL   true   !NULL  - ad hoc prerequisite already updated and blanked
  //
  // Note that we still execute already updated prerequisites to keep the
  // dependency counts straight. But we don't consider them for the "renders
  // us out-of-date" check assuming this has already been done.
  //
  // See also environment::set_special_variables().
  //
  optional<target_state> adhoc_buildscript_rule::
  execute_update_prerequisites (action a, const target& t, timestamp mt) const
  {
    context& ctx (t.ctx);

    // This is essentially a customized execute_prerequisites(a, t, mt).
    //
    size_t busy (ctx.count_busy ());

    target_state rs (target_state::unchanged);

    wait_guard wg (ctx, busy, t[a].task_count);

    auto& pts (t.prerequisite_targets[a]);

    for (const prerequisite_target& p: pts)
    {
      if (const target* pt =
          (p.target != nullptr ? p.target :
           p.adhoc ()          ? reinterpret_cast<target*> (p.data) : nullptr))
      {
        target_state s (execute_async (a, *pt, busy, t[a].task_count));
        assert (s != target_state::postponed);
      }
    }

    wg.wait ();

    bool e (mt == timestamp_nonexistent);
    for (prerequisite_target& p: pts)
    {
      if (const target* pt =
          (p.target != nullptr ? p.target :
           p.adhoc ()          ? reinterpret_cast<target*> (p.data) : nullptr))
      {
        target_state s (execute_complete (a, *pt));

        if (p.data == 0)
        {
          rs |= s;

          // Compare our timestamp to this prerequisite's skipping
          // update=unmatch.
          //
          if (!e && (p.include & 4) == 0)
          {
            // If this is an mtime-based target, then compare timestamps.
            //
            if (const mtime_target* mpt = pt->is_a<mtime_target> ())
            {
              if (mpt->newer (mt, s))
                e = true;
            }
            else
            {
              // Otherwise we assume the prerequisite is newer if it was
              // changed.
              //
              if (s == target_state::changed)
                e = true;
            }
          }

          // Blank out adhoc.
          //
          if (p.adhoc ())
          {
            p.data = reinterpret_cast<uintptr_t> (p.target);
            p.target = nullptr;
          }
        }
      }
    }

    return e ? nullopt : optional<target_state> (rs);
  }

  // Return true if execute_body() was called and thus the caller should call
  // run.leave().
  //
  bool adhoc_buildscript_rule::
  execute_update_file (const scope& bs,
                       action, const file& t,
                       build::script::environment& env,
                       build::script::default_runner& run,
                       bool deferred_failure) const
  {
    context& ctx (t.ctx);

    const scope& rs (*bs.root_scope ());

    // Note that it doesn't make much sense to use the temporary directory
    // variable ($~) in the 'diag' builtin call, so we postpone setting it
    // until the script body execution, that can potentially be omitted.
    //
    build::script::parser p (ctx);

    if (verb == 1)
    {
      if (script.diag_line)
      {
        text << p.execute_special (rs, bs, env, *script.diag_line);
      }
      else
      {
        // @@ TODO (and in default_action() below):
        //
        // - we are printing target, not source (like in most other places)
        //
        // - printing of ad hoc target group (the {hxx cxx}{foo} idea)
        //
        // - if we are printing prerequisites, should we print all of them
        //   (including tools)?
        //
        text << *script.diag_name << ' ' << t;
      }
    }

    if (!ctx.dry_run || verb >= 2)
    {
      // On failure remove the target files that may potentially exist but
      // be invalid.
      //
      small_vector<auto_rmfile, 8> rms;

      if (!ctx.dry_run)
      {
        for (const target* m (&t); m != nullptr; m = m->adhoc_member)
        {
          if (auto* f = m->is_a<file> ())
            rms.emplace_back (f->path ());
        }
      }

      if (script.body_temp_dir && !script.depdb_preamble_temp_dir)
        env.set_temp_dir_variable ();

      p.execute_body (rs, bs,
                      env, script, run,
                      script.depdb_preamble.empty () /* enter */,
                      false                          /* leave */);

      if (!ctx.dry_run)
      {
        if (deferred_failure)
          fail << "expected error exit status from recipe body";

        // If this is an executable, let's be helpful to the user and set
        // the executable bit on POSIX.
        //
#ifndef _WIN32
        auto chmod = [] (const path& p)
        {
          path_perms (p,
                      (path_perms (p)  |
                       permissions::xu |
                       permissions::xg |
                       permissions::xo));
        };

        for (const target* m (&t); m != nullptr; m = m->adhoc_member)
        {
          if (auto* p = m->is_a<exe> ())
            chmod (p->path ());
        }
#endif
        for (auto& rm: rms)
          rm.cancel ();
      }

      return true;
    }
    else
      return false;
  }

  target_state adhoc_buildscript_rule::
  perform_clean_file (action a, const target& t)
  {
    // Besides .d (depdb) also clean .t which is customarily used as a
    // temporary file, such as make dependency output in depdb-dyndep. In
    // fact, initially the plan was to only clean it if we have dyndep but
    // there is no reason it cannot be used for something else.
    //
    // Note that the main advantage of using this file over something in the
    // temporary directory ($~) is that it's next to other output which makes
    // it easier to examine during recipe troubleshooting.
    //
    return perform_clean_extra (a, t.as<file> (), {".d", ".t"});
  }

  target_state adhoc_buildscript_rule::
  default_action (action a,
                  const target& t,
                  const optional<timestamp>& deadline) const
  {
    tracer trace ("adhoc_buildscript_rule::default_action");

    context& ctx (t.ctx);

    execute_prerequisites (a, t);

    if (!ctx.dry_run || verb != 0)
    {
      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      build::script::environment e (a, t, script.body_temp_dir, deadline);
      build::script::parser p (ctx);

      if (verb == 1)
      {
        if (script.diag_line)
        {
          text << p.execute_special (rs, bs, e, *script.diag_line);
        }
        else
        {
          // @@ TODO: as above (execute_update_file()).
          //
          text << *script.diag_name << ' ' << t;
        }
      }

      if (!ctx.dry_run || verb >= 2)
      {
        build::script::default_runner r;
        p.execute_body (rs, bs, e, script, r);
      }
    }

    return target_state::changed;
  }
}
