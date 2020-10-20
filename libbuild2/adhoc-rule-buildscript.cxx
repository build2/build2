// file      : libbuild2/adhoc-rule-buildscript.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/adhoc-rule-buildscript.hxx>

#include <sstream>

#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/parser.hxx> // attributes

#include <libbuild2/build/script/parser.hxx>
#include <libbuild2/build/script/runner.hxx>

using namespace std;

namespace build2
{
  bool adhoc_buildscript_rule::
  recipe_text (context& ctx, const target& tg, string&& t, attributes& as)
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

    istringstream is (move (t));
    build::script::parser p (ctx);

    script = p.pre_parse (tg,
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
      to_stream (os, name (*script.diag_name), true /* quote */, '@');
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

    script::dump (os, ind, script.depdb_lines);

    if (script.diag_line)
    {
      os << ind; script::dump (os, *script.diag_line, true /* newline */);
    }

    script::dump (os, ind, script.lines);
    ind.resize (ind.size () - 2);
    os << ind << string (braces, '}');
  }

  bool adhoc_buildscript_rule::
  match (action a, target& t, const string&, match_extra&,
         optional<action> fb) const
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

  recipe adhoc_buildscript_rule::
  apply (action a, target& t, match_extra&) const
  {
    // If this is an outer operation (e.g., update-for-test), then delegate to
    // the inner.
    //
    if (a.outer ())
    {
      match_inner (a, t);
      return execute_inner;
    }

    // Derive file names for the target and its ad hoc group members, if any.
    //
    if (a == perform_update_id || a == perform_clean_id)
    {
      for (target* m (&t); m != nullptr; m = m->adhoc_member)
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
    inject_fsdir (a, t);

    // Match prerequisites.
    //
    match_prerequisite_members (a, t);

    // See if we are providing the standard clean as a fallback.
    //
    if (t.data<bool> ())
      return &perform_clean_depdb;

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

  target_state adhoc_buildscript_rule::
  perform_update_file (action a, const target& xt) const
  {
    tracer trace ("adhoc_buildscript_rule::perform_update_file");

    context& ctx (xt.ctx);

    const file& t (xt.as<file> ());
    const path& tp (t.path ());

    // How should we hash target and prerequisite sets ($> and $<)? We could
    // hash them as target names (i.e., the same as the $>/< content) or as
    // paths (only for path-based targets). While names feel more general,
    // they are also more expensive to compute. And for path-based targets,
    // path is generally a good proxy for the target name. Since the bulk of
    // the ad hoc recipes will presumably be operating exclusively on
    // path-based targets, let's do it both ways.
    //
    auto hash_target = [ns = names ()] (sha256& cs, const target& t) mutable
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

    // Update prerequisites and determine if any of them render this target
    // out-of-date.
    //
    timestamp mt (t.load_mtime ());
    optional<target_state> ps;

    sha256 pcs, ecs;
    {
      // This is essentially ps=execute_prerequisites(a, t, mt) which we
      // cannot use because we need to see ad hoc prerequisites.
      //
      size_t busy (ctx.count_busy ());
      size_t exec (ctx.count_executed ());

      target_state rs (target_state::unchanged);

      wait_guard wg (ctx, busy, t[a].task_count);

      for (const target*& pt: t.prerequisite_targets[a])
      {
        if (pt == nullptr) // Skipped.
          continue;

        target_state s (execute_async (a, *pt, busy, t[a].task_count));

        if (s == target_state::postponed)
        {
          rs |= s;
          pt = nullptr;
        }
      }

      wg.wait ();

      bool e (mt == timestamp_nonexistent);
      for (prerequisite_target& p: t.prerequisite_targets[a])
      {
        if (p == nullptr)
          continue;

        const target& pt (*p.target);

        const auto& tc (pt[a].task_count);
        if (tc.load (memory_order_acquire) >= busy)
          ctx.sched.wait (exec, tc, scheduler::work_none);

        target_state s (pt.executed_state (a));
        rs |= s;

        // Compare our timestamp to this prerequisite's.
        //
        if (!e)
        {
          // If this is an mtime-based target, then compare timestamps.
          //
          if (const mtime_target* mpt = pt.is_a<mtime_target> ())
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

        if (p.adhoc)
          p.target = nullptr; // Blank out.

        // As part of this loop calculate checksums that need to include ad
        // hoc prerequisites (unless the script tracks changes itself).
        //
        if (script.depdb_clear)
          continue;

        hash_target (pcs, pt);

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
        // When it comes to change tracking, there is nothing we can do for
        // (4) and there is nothing to do for (3) (assuming builtin semantics
        // is stable/backwards-compatible). The (2) case is handled
        // automatically by hashing all the variable values referenced by the
        // script (see below), which in case of process_path_ex includes the
        // checksum, if available.
        //
        // This leaves the (1) case, which itself splits into two sub-cases:
        // the target comes with the dependency information (e.g., imported
        // from a project via an export stub) or it does not (e.g., imported
        // as installed). We don't need to do anything extra for the first
        // sub-case since the target's state/mtime can be relied upon like any
        // other prerequisite. Which cannot be said about the second sub-case,
        // where we reply on checksum that may be included as part of the
        // target metadata.
        //
        // So what we are going to do is hash checksum metadata of every
        // executable prerequisite target that has it (we do it here in order
        // to include ad hoc prerequisites, which feels like the right thing
        // to do; the user may mark tools as ad hoc in order to omit them from
        // $<).
        //
        if (auto* e = pt.is_a<exe> ())
        {
          if (auto* c = e->lookup_metadata<string> ("checksum"))
          {
            ecs.append (*c);
          }
        }
      }

      if (!e)
        ps = rs;
    }

    bool update (!ps);

    // We use depdb to track changes to the script itself, input/output file
    // names, tools, etc.
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
    // script doesn't track the dependency changes itself.
    //

    // For each variable hash its name, undefined/null/non-null indicator,
    // and the value if non-null.
    //
    // Note that this excludes the special $< and $> variables which we
    // handle below.
    //
    if (!script.depdb_clear)
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
    if (!script.depdb_clear)
    {
      sha256 tcs;
      for (const target* m (&t); m != nullptr; m = m->adhoc_member)
        hash_target (tcs, *m);

      if (dd.expect (tcs.string ()) != nullptr)
        l4 ([&]{trace << "target set change forcing update of " << t;});

      if (dd.expect (pcs.string ()) != nullptr)
        l4 ([&]{trace << "prerequisite set change forcing update of " << t;});
    }

    // Finally the programs checksum.
    //
    if (!script.depdb_clear)
    {
      if (dd.expect (ecs.string ()) != nullptr)
        l4 ([&]{trace << "program checksum change forcing update of " << t;});
    }

    const scope* bs (nullptr);
    const scope* rs (nullptr);

    // Execute the custom dependency change tracking commands, if present.
    //
    if (!script.depdb_lines.empty ())
    {
      bs = &t.base_scope ();
      rs = bs->root_scope ();

      // While it would have been nice to reuse the environment for both
      // dependency tracking and execution, there are complications (creating
      // temporary directory, etc).
      //
      build::script::environment e (a, t, false /* temp_dir */);
      build::script::parser p (ctx);

      for (const script::line& l: script.depdb_lines)
      {
        names ns (p.execute_special (*rs, *bs, e, l));

        // These should have been enforced during pre-parsing.
        //
        assert (!ns.empty ());         //         <cmd> ... <newline>
        assert (l.tokens.size () > 2); // 'depdb' <cmd> ... <newline>

        const string& cmd (ns[0].value);

        location loc (l.tokens[0].location ());

        if (cmd == "hash")
        {
          sha256 cs;
          for (auto i (ns.begin () + 1); i != ns.end (); ++i) // Skip <cmd>.
            to_checksum (cs, *i);

          if (dd.expect (cs.string ()) != nullptr)
            l4 ([&] {
                diag_record dr (trace);
                dr << "'depdb hash' argument change forcing update of " << t <<
                  info (loc); script::dump (dr.os, l);
              });
        }
        else if (cmd == "string")
        {
          string s;
          try
          {
            s = convert<string> (names (make_move_iterator (ns.begin () + 1),
                                        make_move_iterator (ns.end ())));
          }
          catch (const invalid_argument& e)
          {
            fail (l.tokens[2].location ())
              << "invalid 'depdb string' argument: " << e;
          }

          if (dd.expect (s) != nullptr)
            l4 ([&] {
                diag_record dr (trace);
                dr << "'depdb string' argument change forcing update of "
                   << t <<
                  info (loc); script::dump (dr.os, l);
              });
        }
        else
          assert (false);
      }
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

    if (!ctx.dry_run || verb != 0)
    {
      if (bs == nullptr)
      {
        bs = &t.base_scope ();
        rs = bs->root_scope ();
      }

      build::script::environment e (a, t, script.temp_dir);
      build::script::parser p (ctx);

      if (verb == 1)
      {
        if (script.diag_line)
        {
          text << p.execute_special (*rs, *bs, e, *script.diag_line);
        }
        else
        {
          // @@ TODO (and below):
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
        build::script::default_runner r;
        p.execute (*rs, *bs, e, script, r);

        if (!ctx.dry_run)
          dd.check_mtime (tp);
      }
    }

    t.mtime (system_clock::now ());
    return target_state::changed;
  }

  target_state adhoc_buildscript_rule::
  default_action (action a, const target& t) const
  {
    tracer trace ("adhoc_buildscript_rule::default_action");

    context& ctx (t.ctx);

    execute_prerequisites (a, t);

    if (!ctx.dry_run || verb != 0)
    {
      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      build::script::environment e (a, t, script.temp_dir);
      build::script::parser p (ctx);

      if (verb == 1)
      {
        if (script.diag_line)
        {
          text << p.execute_special (rs, bs, e, *script.diag_line);
        }
        else
        {
          // @@ TODO: as above
          //
          text << *script.diag_name << ' ' << t;
        }
      }

      if (!ctx.dry_run || verb >= 2)
      {
        build::script::default_runner r;
        p.execute (rs, bs, e, script, r);
      }
    }

    return target_state::changed;
  }
}
