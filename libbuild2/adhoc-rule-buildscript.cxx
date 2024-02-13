// file      : libbuild2/adhoc-rule-buildscript.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/adhoc-rule-buildscript.hxx>

#include <sstream>

#include <libbutl/filesystem.hxx> // try_rm_file(), path_entry()

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
                    const scope& bs,
                    const target& t,
                    names& storage)
  {
    auto& vp (bs.var_pool ());

    for (const string& n: s.vars)
    {
      cs.append (n);

      lookup l;

      if (const variable* var = vp.find (n))
        l = t[var];

      cs.append (!l.defined () ? '\x1' : l->null ? '\x2' : '\x3');

      if (l)
      {
        storage.clear ();
        names_view ns (reverse (*l, storage, true /* reduce */));

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
    script::dump (os, ind, script.diag_preamble);

    script::dump (os, ind, script.body);
    ind.resize (ind.size () - 2);
    os << ind << string (braces, '}');
  }

  bool adhoc_buildscript_rule::
  reverse_fallback (action a, const target_type& tt) const
  {
    // We can provide clean for a file or group target if we are providing
    // update.
    //
    return (a == perform_clean_id                   &&
            (tt.is_a<file> () || tt.is_a<group> ()) &&
            find (actions.begin (), actions.end (),
                  perform_update_id) != actions.end ());
  }

  using dynamic_target = build::script::parser::dynamic_target;
  using dynamic_targets = build::script::parser::dynamic_targets;

  struct adhoc_buildscript_rule::match_data
  {
    match_data (action a, const target& t, const scope& bs, bool temp_dir)
        : env (a, t, bs, temp_dir) {}

    build::script::environment env;
    build::script::default_runner run;

    path dd;
    dynamic_targets dyn_targets;

    const scope* bs;
    timestamp mt;
    bool deferred_failure;
  };

  struct adhoc_buildscript_rule::match_data_byproduct
  {
    match_data_byproduct (action a, const target& t,
                          const scope& bs,
                          bool temp_dir)
        : env (a, t, bs, temp_dir) {}

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
  match (action a, target& xt, const string& h, match_extra& me) const
  {
    const target& t (xt); // See adhoc_rule::match().

    // We pre-parsed the script with the assumption it will be used on a
    // non/file-based (or file group-based) target. Note that this should not
    // be possible with patterns.
    //
    if (pattern == nullptr)
    {
      // Let's not allow mixing file/group.
      //
      if ((t.is_a<file> () != nullptr) == ttype->is_a<file> () ||
          (t.is_a<group> () != nullptr) == ttype->is_a<group> ())
        ;
      else
        fail (loc) << "incompatible target types used with shared recipe" <<
          info << "all targets must be file- or file group-based or non";
    }

    return adhoc_rule::match (a, xt, h, me);
  }

  recipe adhoc_buildscript_rule::
  apply (action a, target& t, match_extra& me) const
  {
    return apply (a, t, me, nullopt);
  }

  recipe adhoc_buildscript_rule::
  apply (action a,
         target& t,
         match_extra& me,
         const optional<timestamp>& deadline) const
  {
    tracer trace ("adhoc_buildscript_rule::apply");

    // Handle matching group members (see adhoc_rule::match() for background).
    //
    if (const group* g = t.group != nullptr ? t.group->is_a<group> () : nullptr)
    {
      // Note: this looks very similar to how we handle ad hoc group members.
      //
      match_sync (a, *g, 0 /* options */);
      return group_recipe; // Execute the group's recipe.
    }

    // We don't support deadlines for any of these cases (see below).
    //
    if (deadline && (a.outer ()  ||
                     me.fallback ||
                     (a == perform_update_id &&
                      (t.is_a<file> () || t.is_a<group> ()))))
      return empty_recipe;

    // If this is an outer operation (e.g., update-for-test), then delegate to
    // the inner.
    //
    if (a.outer ())
    {
      match_inner (a, t);
      return inner_recipe;
    }

    context& ctx (t.ctx);
    const scope& bs (t.base_scope ());

    group* g (t.is_a<group> ()); // Explicit group.

    // Inject pattern's ad hoc group members, if any (explicit group members
    // are injected after reset below).
    //
    if (g == nullptr && pattern != nullptr)
      pattern->apply_group_members (a, t, bs, me);

    // Derive file names for the target and its static/ad hoc group members,
    // if any.
    //
    if (a == perform_update_id || a == perform_clean_id)
    {
      if (g != nullptr)
      {
        g->reset_members (a); // See group::group_members() for background.

        // Note that we rely on the fact that if the group has static members,
        // then they always come first in members and the first static member
        // is a file.
        //
        for (const target& m: g->static_members)
          g->members.push_back (&m);

        g->members_static = g->members.size ();

        if (pattern != nullptr)
        {
          pattern->apply_group_members (a, *g, bs, me);
          g->members_static = g->members.size ();
        }

        if (g->members_static == 0)
        {
          if (!script.depdb_dyndep_dyn_target)
            fail << "group " << *g << " has no static or dynamic members";
        }
        else
        {
          if (!g->members.front ()->is_a<file> ())
          {
            // We use the first static member to derive depdb path, get mtime,
            // etc. So it must be file-based.
            //
            fail << "first static member " << g->members.front ()
                 << " of group " << *g << " is not a file";
          }

          // Derive paths for all the static members.
          //
          for (const target* m: g->members)
            if (auto* p = m->is_a<path_target> ())
              p->derive_path ();
        }
      }
      else
      {
        for (target* m (&t); m != nullptr; m = m->adhoc_member)
        {
          if (auto* p = m->is_a<path_target> ())
            p->derive_path ();
        }
      }
    }
    else if (g != nullptr)
    {
      // This could be, for example, configure/dist update which could need a
      // "representative sample" of members (in order to be able to match the
      // rules). So add static members unless we already have something
      // cached.
      //
      if (g->group_members (a).members == nullptr) // Note: not g->member.
      {
        g->reset_members (a);

        for (const target& m: g->static_members)
          g->members.push_back (&m);

        g->members_static = g->members.size ();

        if (pattern != nullptr)
        {
          pattern->apply_group_members (a, *g, bs, me);
          g->members_static = g->members.size ();
        }
      }
    }

    // Inject dependency on the output directory.
    //
    // We do it always instead of only if one of the targets is path-based in
    // case the recipe creates temporary files or some such.
    //
    // Note that we disable the prerequisite search for fsdir{} because of the
    // prerequisites injected by the pattern. So we have to handle this ad hoc
    // below.
    //
    const fsdir* dir (inject_fsdir (a, t, true /*match*/, false /*prereq*/));

    // Match prerequisites.
    //
    // This is essentially match_prerequisite_members() but with support
    // for update=unmatch|match.
    //
    auto& pts (t.prerequisite_targets[a]);
    {
      // Re-create the clean semantics as in match_prerequisite_members().
      //
      bool clean (a.operation () == clean_id && !t.is_a<alias> ());

      // Add target's prerequisites.
      //
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // Note that we have to recognize update=unmatch|match for *(update),
        // not just perform(update). But only actually do anything about it
        // for perform(update).
        //
        lookup l; // The `update` variable value, if any.
        include_type pi (
          include (a, t, p, a.operation () == update_id ? &l : nullptr));

        // Use prerequisite_target::include to signal update during match or
        // unmatch.
        //
        uintptr_t mask (0);
        if (l)
        {
          const string& v (cast<string> (l));

          if (v == "match")
          {
            if (a == perform_update_id)
              mask = prerequisite_target::include_udm;
          }
          else if (v == "unmatch")
          {
            if (a == perform_update_id)
              mask = include_unmatch;
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

        const target& pt (p.search (t));

        if (&pt == dir) // Don't add injected fsdir{} twice.
          continue;

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
        pattern->apply_prerequisites (a, t, bs, me);

      // Start asynchronous matching of prerequisites. Wait with unlocked
      // phase to allow phase switching.
      //
      wait_guard wg (ctx, ctx.count_busy (), t[a].task_count, true);

      for (const prerequisite_target& pt: pts)
      {
        if (pt.target == dir) // Don't match injected fsdir{} twice.
          continue;

        match_async (a, *pt.target, ctx.count_busy (), t[a].task_count);
      }

      wg.wait ();

      // Finish matching all the targets that we have started.
      //
      for (prerequisite_target& pt: pts)
      {
        if (pt.target == dir) // See above.
          continue;

        // Handle update=unmatch.
        //
        unmatch um ((pt.include & include_unmatch) != 0
                    ? unmatch::safe
                    : unmatch::none);

        pair<bool, target_state> mr (match_complete (a, *pt.target, um));

        if (um != unmatch::none)
        {
          l6 ([&]{trace << "unmatch " << *pt.target << ": " << mr.first;});

          // If we managed to unmatch, blank it out so that it's not executed,
          // etc. Otherwise, leave it as is (but we still automatically avoid
          // hashing it, updating it during match in exec_depdb_dyndep(), and
          // making us out of date in execute_update_prerequisites()).
          //
          // The hashing part is tricky: by not hashing it we won't detect the
          // case where it was removed as a prerequisite altogether. The
          // thinking is that it was added with update=unmatch to extract some
          // information (e.g., poptions from a library) and those will be
          // change-tracked.
          //
          // Note: set the include_target flag for the updated_during_match()
          // check.
          //
          if (mr.first)
          {
            pt.data = reinterpret_cast<uintptr_t> (pt.target);
            pt.target = nullptr;
            pt.include |= prerequisite_target::include_target;

            // Note that this prerequisite could also be ad hoc and we must
            // clear that flag if we managed to unmatch (failed that we will
            // treat it as ordinary ad hoc since it has the target pointer in
            // data).
            //
            // But that makes it impossible to distinguish ad hoc unmatch from
            // ordinary unmatch prerequisites later when setting $<. Another
            // flag to the rescue.
            //
            if ((pt.include & prerequisite_target::include_adhoc) != 0)
            {
              pt.include &= ~prerequisite_target::include_adhoc;
              pt.include |= include_unmatch_adhoc;
            }
          }
        }
      }
    }

    // Read the list of dynamic targets and, optionally, fsdir{} prerequisites
    // from depdb, if exists (used in a few depdb-dyndep --dyn-target handling
    // places below).
    //
    auto read_dyn_targets = [] (path ddp, bool fsdir)
      -> pair<dynamic_targets, dir_paths>
    {
      depdb dd (move (ddp), true /* read_only */);

      pair<dynamic_targets, dir_paths> r;
      while (dd.reading ()) // Breakout loop.
      {
        string* l;
        auto read = [&dd, &l] () -> bool
        {
          return (l = dd.read ()) != nullptr;
        };

        if (!read ()) // Rule id.
          break;

        // We can omit this for as long as we don't break our blank line
        // anchors semantics.
        //
#if 0
        if (*l != rule_id_)
          fail << "unable to clean dynamic target group " << t
               << " with old depdb";
#endif

        // Note that we cannot read out expected lines since there can be
        // custom depdb builtins. So we use the blank lines as anchors to
        // skip to the parts we need.
        //
        // Skip until the first blank that separated custom depdb entries from
        // the prerequisites list.
        {
          bool g;
          while ((g = read ()) && !l->empty ()) ;
          if (!g)
            break;
        }

        // Next read the prerequisites, detecting fsdir{} entries if asked.
        //
        {
          bool g;
          while ((g = read ()) && !l->empty ())
          {
            if (fsdir)
            {
              path p (*l);
              if (p.to_directory ())
                r.second.push_back (path_cast<dir_path> (move (p)));
            }
          }

          if (!g)
            break;
        }

        // Read the dynamic target files. We should always end with a blank
        // line.
        //
        for (;;)
        {
          if (!read () || l->empty ())
            break;

          // Split into type and path.
          //
          size_t p (l->find (' '));
          if (p == string::npos || // Invalid format.
              p == 0            || // Empty type.
              p + 1 == l->size ()) // Empty path.
            break;

          r.first.push_back (
            dynamic_target {string (*l, 0, p), path (*l, p + 1, string::npos)});
        }

        break;
      }

      return r;
    };

    // Target path to derive the depdb path, query mtime (if file), etc.
    //
    // To derive the depdb path for a group with at least one static member we
    // use the path of the first member. For a group without any static
    // members we use the group name with the target type name as the
    // second-level extension.
    //
    auto target_path = [&t, g, p = path ()] () mutable -> const path&
    {
      return
      g == nullptr           ? t.as<file> ().path ()                    :
      g->members_static != 0 ? g->members.front ()->as<file> ().path () :
      (p = g->dir / (g->name + '.' + g->type ().name));
    };

    // See if we are providing the standard clean as a fallback.
    //
    if (me.fallback)
    {
      // For depdb-dyndep --dyn-target use depdb to clean dynamic targets.
      //
      if (script.depdb_dyndep_dyn_target)
      {
        // Note that only removing the relevant filesystem entries is not
        // enough: we actually have to populate the group with members since
        // this information could be used to clean derived targets (for
        // example, object files). So we just do that and let the standard
        // clean logic take care of them the same as static members.
        //
        // NOTE that this logic should be consistent with what we have in
        // exec_depdb_dyndep().
        //
        using dyndep = dyndep_rule;

        function<dyndep::group_filter_func> filter;
        if (g != nullptr)
        {
          filter = [] (mtime_target& g, const build2::file& m)
          {
            auto& ms (g.as<group> ().members);
            return find (ms.begin (), ms.end (), &m) == ms.end ();
          };
        }

        pair<dynamic_targets, dir_paths> p (
          read_dyn_targets (target_path () + ".d", true));

        for (dynamic_target& dt: p.first)
        {
          path& f (dt.path);

          // Resolve target type. Clean it as file if unable to.
          //
          const target_type* tt (bs.find_target_type (dt.type));
          if (tt == nullptr)
            tt = &file::static_type;

          if (g != nullptr)
          {
            pair<const build2::file&, bool> r (
              dyndep::inject_group_member (a, bs, *g, move (f), *tt, filter));

            if (r.second)
              g->members.push_back (&r.first);
          }
          else
          {
            // Note that here we don't bother cleaning any old dynamic targets
            // -- the more we can clean, the merrier.
            //
            dyndep::inject_adhoc_group_member (a, bs, t, move (f), *tt);
          }
        }

        // Enter fsdir{} prerequisites.
        //
        // See the add lambda in exec_depdb_dyndep() for background.
        //
        for (dir_path& d: p.second)
        {
          dir_path o; string n; // For GCC 13 -Wdangling-reference.
          const fsdir& dt (search<fsdir> (t,
                                          move (d),
                                          move (o),
                                          move (n), nullptr, nullptr));
          match_sync (a, dt);
          pts.push_back (prerequisite_target (&dt, true /* adhoc */));
        }
      }

      return g == nullptr ? perform_clean_file : perform_clean_group;
    }

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
      update_during_match_prerequisites (trace, a, t);

    // See if this is not update or not on a file/group-based target.
    //
    if (a != perform_update_id || !(g != nullptr || t.is_a<file> ()))
    {
      // Make sure we get small object optimization.
      //
      if (deadline)
      {
        return [dv = *deadline, this] (action a, const target& t)
        {
          return default_action (a, t, dv);
        };
      }
      else
      {
        return [this] (action a, const target& t)
        {
          return default_action (a, t, nullopt);
        };
      }
    }

    // This is a perform update on a file or group target.
    //
    // See if this is the simple case with only static dependencies.
    //
    if (!script.depdb_dyndep)
    {
      return [this] (action a, const target& t)
      {
        return perform_update_file_or_group (a, t);
      };
    }

    // This is a perform update on a file or group target with extraction of
    // dynamic dependency information either in the depdb preamble
    // (depdb-dyndep without --byproduct) or as a byproduct of the recipe body
    // execution (depdb-dyndep with --byproduct).
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

    // Re-acquire fsdir{} specified by the user, similar to inject_fsdir()
    // (which we have disabled; see above).
    //
    if (dir == nullptr)
    {
      for (const target* pt: pts)
      {
        if (pt != nullptr)
        {
          if (const fsdir* dt = pt->is_a<fsdir> ())
          {
            if (dt->dir == t.dir)
            {
              dir = dt;
              break;
            }
          }
        }
      }
    }

    if (dir != nullptr)
      fsdir_rule::perform_update_direct (a, *dir);

    // Because the depdb preamble can access $<, we have to blank out all the
    // ad hoc prerequisites. Since we will still need them later, we "move"
    // them to the auxiliary data member in prerequisite_target (see
    // execute_update_prerequisites() for details).
    //
    // Note: set the include_target flag for the updated_during_match() check.
    //
    for (prerequisite_target& p: pts)
    {
      // Note that fsdir{} injected above is adhoc.
      //
      if (p.target != nullptr && p.adhoc ())
      {
        p.data = reinterpret_cast<uintptr_t> (p.target);
        p.target = nullptr;
        p.include |= prerequisite_target::include_target;
      }
    }

    const path& tp (target_path ());

    // Note that while it's tempting to turn match_data* into recipes, some of
    // their members are not movable. And in the end we will have the same
    // result: one dynamic memory allocation.
    //
    unique_ptr<match_data> md;
    unique_ptr<match_data_byproduct> mdb;

    dynamic_targets old_dyn_targets;

    if (script.depdb_dyndep_byproduct)
    {
      mdb.reset (new match_data_byproduct (
                   a, t, bs, script.depdb_preamble_temp_dir));
    }
    else
    {
      md.reset (new match_data (a, t, bs, script.depdb_preamble_temp_dir));

      // If the set of dynamic targets can change based on changes to the
      // inputs (say, each entity, such as a type, in the input file gets its
      // own output file), then we can end up with a large number of old
      // output files laying around because they are not part of the new
      // dynamic target set. So we try to clean them up based on the old depdb
      // information, similar to how we do it for perform_clean above (except
      // here we will just keep the list of old files).
      //
      // Note: do before opening depdb, which can start over-writing it.
      //
      // We also have to do this speculatively, without knowing whether we
      // will need to update. Oh, well, being dynamic ain't free.
      //
      if (script.depdb_dyndep_dyn_target)
        old_dyn_targets = read_dyn_targets (tp + ".d", false).first;
    }

    depdb dd (tp + ".d");

    // NOTE: see the "static dependencies" version (with comments) below.
    //
    // NOTE: We use blank lines as anchors to skip directly to certain entries
    //       (e.g., dynamic targets). So make sure none of the other entries
    //       can be blank (for example, see `depdb string` builtin).
    //
    // NOTE: KEEP IN SYNC WITH read_dyn_targets ABOVE!
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
          if ((p.include & include_unmatch) != 0) // Skip update=unmatch.
            continue;

          hash_prerequisite_target (prq_cs, exe_cs, env_cs, *pt, storage);
        }
      }

      {
        sha256 cs;
        hash_script_vars (cs, script, bs, t, storage);

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "recipe variable change forcing update of " << t;});
      }

      // Static targets and prerequisites (there can also be dynamic targets;
      // see dyndep --dyn-target).
      //
      {
        sha256 tcs;
        if (g == nullptr)
        {
          // There is a nuance: in an operation batch (e.g., `b update
          // update`) we will already have the dynamic targets as members on
          // the subsequent operations and we need to make sure we don't treat
          // them as static. Using target_decl to distinguish the two seems
          // like a natural way.
          //
          for (const target* m (&t); m != nullptr; m = m->adhoc_member)
          {
            if (m->decl == target_decl::real)
              hash_target (tcs, *m, storage);
          }
        }
        else
        {
          // Feels like there is not much sense in hashing the group itself.
          //
          for (const target* m: g->members)
            hash_target (tcs, *m, storage);
        }

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

    // Get ready to run the depdb preamble.
    //
    build::script::environment& env (mdb != nullptr ? mdb->env : md->env);
    build::script::default_runner& run (mdb != nullptr ? mdb->run : md->run);

    run.enter (env, script.start_loc);

    // Run the first half of the preamble (before depdb-dyndep).
    //
    {
      build::script::parser p (ctx);
      p.execute_depdb_preamble (a, bs, t, env, script, run, dd);

      // Write a blank line after the custom depdb entries and before
      // prerequisites, which we use as an anchor (see read_dyn_targets
      // above). We only do it for the new --dyn-target mode in order not to
      // invalidate the existing depdb instances.
      //
      if (script.depdb_dyndep_dyn_target)
        dd.expect ("");
    }

    // Determine if we need to do an update based on the above checks.
    //
    bool update (false);
    timestamp mt;

    if (dd.writing ())
      update = true;
    else
    {
      if (g == nullptr)
      {
        const file& ft (t.as<file> ());

        if ((mt = ft.mtime ()) == timestamp_unknown)
          ft.mtime (mt = mtime (tp)); // Cache.
      }
      else
      {
        // Use static member, old dynamic, or force update.
        //
        const path* p (
          g->members_static != 0
          ? &tp /* first static member path */
          : (!old_dyn_targets.empty ()
             ? &old_dyn_targets.front ().path
             : nullptr));

        if (p != nullptr)
          mt = g->load_mtime (*p);
        else
          update = true;
      }

      if (!update)
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
      // update if we are already updating (or not: there is pre-generation
      // to consider; see inject_existing_file() for details).
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
                    a, &bs, &t, pts_n = mdb->pts_n,
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
                  a, t, pts_n,
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
      if (!update || ctx.dry_run_option)
        dd.close (false /* mtime_check */);
      else
        mdb->dd = dd.close_to_reopen ();

      // Pass on base scope and update/mtime.
      //
      mdb->bs = &bs;
      mdb->mt = update ? timestamp_nonexistent : mt;

      return [this, md = move (mdb)] (action a, const target& t)
      {
        return perform_update_file_or_group_dyndep_byproduct (a, t, *md);
      };
    }
    else
    {
      // Run the second half of the preamble (depdb-dyndep commands) to update
      // our prerequisite targets and extract dynamic dependencies (targets
      // and prerequisites).
      //
      // Note that this should be the last update to depdb (the invalidation
      // order semantics).
      //
      md->deferred_failure = false;
      {
        build::script::parser p (ctx);
        p.execute_depdb_preamble_dyndep (a, bs, t,
                                         env, script, run,
                                         dd,
                                         md->dyn_targets,
                                         update,
                                         mt,
                                         md->deferred_failure);
      }

      if (update && dd.reading () && !ctx.dry_run_option)
        dd.touch = timestamp_unknown;

      dd.close (false /* mtime_check */);

      // Remove previous dynamic targets since their set may change with
      // changes to the inputs.
      //
      // The dry-run mode complicates things: if we don't remove the old
      // files, then that information will be gone (since we update depdb even
      // in the dry-run mode). But if we remove everything in the dry-run
      // mode, then we may also remove some of the current files, which would
      // be incorrect. So let's always remove but only files that are not in
      // the current set.
      //
      // Note that we used to do this in perform_update_file_or_group_dyndep()
      // but that had a tricky issue: if we end up performing match but not
      // execute (e.g., via the resolve_members() logic), then we will not
      // cleanup old targets but loose this information (since the depdb has
      // be updated). So now we do it here, which is a bit strange, but it
      // sort of fits into that dry-run logic above. Note also that we do this
      // unconditionally, update or not, since if everything is up to date,
      // then old and new sets should be the same.
      //
      for (const dynamic_target& dt: old_dyn_targets)
      {
        const path& f (dt.path);

        if (find_if (md->dyn_targets.begin (), md->dyn_targets.end (),
                     [&f] (const dynamic_target& dt)
                     {
                       return dt.path == f;
                     }) == md->dyn_targets.end ())
        {
          // This is an optimization so best effort.
          //
          if (optional<rmfile_status> s = butl::try_rmfile_ignore_error (f))
          {
            if (s == rmfile_status::success && verb >= 2)
              text << "rm " << f;
          }
        }
      }

      // Pass on the base scope, depdb path, and update/mtime.
      //
      md->bs = &bs;
      md->dd = move (dd.path);
      md->mt = update ? timestamp_nonexistent : mt;

      return [this, md = move (md)] (action a, const target& t)
      {
        return perform_update_file_or_group_dyndep (a, t, *md);
      };
    }
  }

  target_state adhoc_buildscript_rule::
  perform_update_file_or_group_dyndep_byproduct (
    action a, const target& t, match_data_byproduct& md) const
  {
    // Note: using shared function name among the three variants.
    //
    tracer trace (
      "adhoc_buildscript_rule::perform_update_file_or_group_dyndep_byproduct");

    context& ctx (t.ctx);

    // For a group we use the first (for now static) member as a source of
    // mtime.
    //
    // @@ TODO: expl: byproduct: Note that until we support dynamic targets in
    // the byproduct mode, we verify there is at least one static member in
    // apply() above. Once we do support this, we will need to verify after
    // the dependency extraction below.
    //
    const group* g (t.is_a<group> ());

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
      if (g == nullptr)
        execute_update_file (bs, a, t.as<file> (), env, run);
      else
      {
        // Note: no dynamic members yet.
        //
        execute_update_group (bs, a, *g, env, run);
      }
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
                  a, &bs, &t, g, &pts, pts_n = md.pts_n,
                  &byp, &map_ext, &dd, &skip] (path fp)
      {
        normalize_external (fp, what);

        // Note that unless we take into account dynamic targets, the skip
        // logic below falls apart since we neither see targets entered via
        // prerequsites (skip static prerequisites) nor by the cache=true code
        // above (skip depdb entries).
        //
        // If this turns out to be racy (which is the reason we would skip
        // dynamic targets; see the fine_file() implementation for details),
        // then the only answer for now is to not use the byproduct mode.
        //
        if (const build2::file* ft = dyndep::find_file (
              trace, what,
              a, bs, t,
              fp, false /* cache */, true /* normalized */,
              true /* dynamic */,
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

          // Skip if this is one of the targets (see the non-byproduct version
          // for background).
          //
          if (byp.drop_cycles)
          {
            if (g != nullptr)
            {
              auto& ms (g->members);
              if (find (ms.begin (), ms.end (), ft) != ms.end ())
                return;
            }
            else
            {
              for (const target* m (&t); m != nullptr; m = m->adhoc_member)
              {
                if (ft == m)
                  return;
              }
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
          dyndep::verify_existing_file (trace, what, a, t, pts_n, *ft);
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

              // Note: no support for dynamic targets in byproduct mode.
              //
              if (r.first == make_type::target)
                continue;

              path& f (r.second);

              if (f.relative ())
              {
                if (!byp.cwd)
                  fail (il) << "relative " << what
                            << " prerequisite path '" << f
                            << "' in make dependency declaration" <<
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
      case dyndep_format::lines:
        {
          for (string l;; ++il.line) // Reuse the buffer.
          {
            if (eof (getline (is, l)))
              break;

            if (l.empty ())
              fail (il) << "blank line in prerequisites list";

            if (l.front () == ' ')
              fail (il) << "non-existent prerequisite in --byproduct mode";

            path f;
            try
            {
              f = path (l);

              // fsdir{} prerequisites only make sense with dynamic targets.
              //
              if (f.to_directory ())
                throw invalid_path ("");

              if (f.relative ())
              {
                if (!byp.cwd)
                  fail (il) << "relative " << what
                            << " prerequisite path '" << f
                            << "' in lines dependency declaration" <<
                    info << "consider using --cwd to specify "
                         << "relative path base";

                f = *byp.cwd / f;
              }
            }
            catch (const invalid_path&)
            {
              fail (il) << "invalid " << what << " prerequisite path '"
                        << l << "'";
            }

            add (move (f));
          }

          break;
        }
      }

      // Add the terminating blank line.
      //
      dd.expect ("");
      dd.close ();

      //@@ TODO: expl: byproduct: verify have at least one member.

      md.dd.path = move (dd.path); // For mtime check below.
    }

    run.leave (env, script.end_loc);

    timestamp now (system_clock::now ());

    if (!ctx.dry_run)
    {
      // Only now we know for sure there must be a member in the group.
      //
      const file& ft ((g == nullptr ? t : *g->members.front ()).as<file> ());

      depdb::check_mtime (start, md.dd.path, ft.path (), now);
    }

    (g == nullptr
     ? static_cast<const mtime_target&> (t.as<file> ())
     : static_cast<const mtime_target&> (*g)).mtime (now);

    return target_state::changed;
  }

  target_state adhoc_buildscript_rule::
  perform_update_file_or_group_dyndep (
    action a, const target& t, match_data& md) const
  {
    tracer trace (
      "adhoc_buildscript_rule::perform_update_file_or_group_dyndep");

    context& ctx (t.ctx);

    // For a group we use the first (static or dynamic) member as a source of
    // mtime. Note that in this case there must be at least one since we fail
    // if we were unable to extract any dynamic members and there are no
    // static (see exec_depdb_dyndep()).
    //
    const group* g (t.is_a<group> ());

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
      if (g == nullptr)
        execute_update_file (
          *md.bs, a, t.as<file> (), env, run, md.deferred_failure);
      else
        execute_update_group (*md.bs, a, *g, env, run, md.deferred_failure);
    }

    run.leave (env, script.end_loc);

    timestamp now (system_clock::now ());

    if (!ctx.dry_run)
    {
      // Note: in case of deferred failure we may not have any members.
      //
      const file& ft ((g == nullptr ? t : *g->members.front ()).as<file> ());
      depdb::check_mtime (start, md.dd, ft.path (), now);
    }

    (g == nullptr
     ? static_cast<const mtime_target&> (t)
     : static_cast<const mtime_target&> (*g)).mtime (now);

    return target_state::changed;
  }

  target_state adhoc_buildscript_rule::
  perform_update_file_or_group (action a, const target& t) const
  {
    tracer trace ("adhoc_buildscript_rule::perform_update_file_or_group");

    context& ctx (t.ctx);
    const scope& bs (t.base_scope ());

    // For a group we use the first (static) member to derive depdb path, as a
    // source of mtime, etc. Note that in this case there must be a static
    // member since in this version of perform_update we don't extract dynamic
    // dependencies (see apply() details).
    //
    const group* g (t.is_a<group> ());

    const file& ft ((g == nullptr ? t : *g->members.front ()).as<file> ());
    const path& tp (ft.path ());

    // Support creating file symlinks using ad hoc recipes.
    //
    auto path_symlink = [&tp] ()
    {
      pair<bool, butl::entry_stat> r (
        butl::path_entry (tp,
                          false /* follow_symlinks */,
                          true /* ignore_errors */));

      return r.first && r.second.type == butl::entry_type::symlink;
    };

    // Update prerequisites and determine if any of them render this target
    // out-of-date.
    //
    // If the file entry exists, check if its a symlink.
    //
    bool symlink (false);
    timestamp mt;

    if (g == nullptr)
    {
      mt = ft.load_mtime ();

      if (mt != timestamp_nonexistent)
        symlink = path_symlink ();
    }
    else
      mt = g->load_mtime (tp);

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
          if ((p.include & include_unmatch) != 0) // Skip update=unmatch.
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
        hash_script_vars (cs, script, bs, t, storage);

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "recipe variable change forcing update of " << t;});
      }

      // Target and prerequisite sets ($> and $<).
      //
      {
        sha256 tcs;
        if (g == nullptr)
        {
          for (const target* m (&t); m != nullptr; m = m->adhoc_member)
            hash_target (tcs, *m, storage);
        }
        else
        {
          // Feels like there is not much sense in hashing the group itself.
          //
          for (const target* m: g->members)
            hash_target (tcs, *m, storage);
        }

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
      // If this is a symlink, depdb mtime could be greater than the symlink
      // target.
      //
      if (dd.writing () || (dd.mtime > mt && !symlink))
        update = true;

      if (!update)
      {
        dd.close ();
        return *ps;
      }
    }

    build::script::environment env (a, t, bs, false /* temp_dir */);
    build::script::default_runner run;

    if (depdb_preamble)
    {
      if (script.depdb_preamble_temp_dir)
        env.set_temp_dir_variable ();

      build::script::parser p (ctx);

      run.enter (env, script.start_loc);
      p.execute_depdb_preamble (a, bs, t, env, script, run, dd);
    }

    // Update if depdb mismatch.
    //
    if (dd.writing () || (dd.mtime > mt && !symlink))
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
      // Prepare to execute the script diag preamble and/or body.
      //
      r = g == nullptr
        ? execute_update_file (bs, a, ft, env, run)
        : execute_update_group (bs, a, *g, env, run);

      if (r)
      {
        if (!ctx.dry_run)
        {
          if (g == nullptr)
            symlink = path_symlink ();

          // Again, if this is a symlink, depdb mtime will be greater than
          // the symlink target.
          //
          if (!symlink)
            dd.check_mtime (tp);
        }
      }
    }

    if (r || depdb_preamble)
      run.leave (env, script.end_loc);

    // Symlinks don't play well with dry-run: we can't extract accurate target
    // timestamp without creating the symlink. Overriding the dry-run doesn't
    // seem to be an option since we don't know whether it will be a symlink
    // until it's created. At least we are being pessimistic rather than
    // optimistic here.
    //
    (g == nullptr
     ? static_cast<const mtime_target&> (ft)
     : static_cast<const mtime_target&> (*g)).mtime (
       symlink
       ? build2::mtime (tp)
       : system_clock::now ());

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
  //  NULL   false  !NULL  - unmatched prerequisite (ignored by this function)
  //
  // Note that we still execute already updated prerequisites to keep the
  // dependency counts straight. But we don't consider them for the "renders
  // us out-of-date" check assuming this has already been done.
  //
  // See also environment::set_special_variables().
  //
  // See also perform_execute() which has to deal with these shenanigans.
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
          if (!e && (p.include & include_unmatch) == 0)
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
          // Note: set the include_target flag for the updated_during_match()
          // check.
          //
          if (p.adhoc ())
          {
            p.data = reinterpret_cast<uintptr_t> (p.target);
            p.target = nullptr;
            p.include |= prerequisite_target::include_target;
          }
        }
      }
    }

    return e ? nullopt : optional<target_state> (rs);
  }

  // Return true if execute_diag_preamble() and/or execute_body() were called
  // and thus the caller should call run.leave().
  //
  bool adhoc_buildscript_rule::
  execute_update_file (const scope& bs,
                       action a, const file& t,
                       build::script::environment& env,
                       build::script::default_runner& run,
                       bool deferred_failure) const
  {
    // NOTE: similar to execute_update_group() below.
    //
    context& ctx (t.ctx);

    const scope& rs (*bs.root_scope ());

    // Note that it doesn't make much sense to use the temporary directory
    // variable ($~) in the 'diag' builtin call, so we postpone setting it
    // until the script body execution, that can potentially be omitted.
    //
    build::script::parser p (ctx);

    bool exec_body  (!ctx.dry_run || verb >= 2);
    bool exec_diag  (!script.diag_preamble.empty () && (exec_body || verb == 1));
    bool exec_depdb (!script.depdb_preamble.empty ());

    if (script.diag_name)
    {
      if (verb == 1)
      {
        // By default we print the first non-ad hoc prerequisite target as the
        // "main" prerequisite, unless there isn't any or it's not file-based,
        // in which case we fallback to the second form without the
        // prerequisite. Potential future improvements:
        //
        // - Somehow detect that the first prerequisite target is a tool being
        //   executed and fallback to the second form. It's tempting to just
        //   exclude all exe{} targets, but this could be a rule for something
        //   like strip.
        //
        const file* pt (nullptr);
        for (const prerequisite_target& p: t.prerequisite_targets[a])
        {
          // See execute_update_prerequisites().
          //
          if (p.target != nullptr && !p.adhoc ())
          {
            pt = p.target->is_a<file> ();
            break;
          }
        }

        if (t.adhoc_member == nullptr)
        {
          if (pt != nullptr)
            print_diag (script.diag_name->c_str (), *pt, t);
          else
            print_diag (script.diag_name->c_str (), t);
        }
        else
        {
          vector<target_key> ts;
          for (const target* m (&t); m != nullptr; m = m->adhoc_member)
            ts.push_back (m->key ());

          if (pt != nullptr)
            print_diag (script.diag_name->c_str (), pt->key (), move (ts));
          else
            print_diag (script.diag_name->c_str (), move (ts));
        }
      }
    }
    else if (exec_diag)
    {
      if (script.diag_preamble_temp_dir && !script.depdb_preamble_temp_dir)
        env.set_temp_dir_variable ();

      pair<names, location> diag (
        p.execute_diag_preamble (rs, bs,
                                 env, script, run,
                                 verb == 1   /* diag */,
                                 !exec_depdb /* enter */,
                                 false       /* leave */));

      if (verb == 1)
        print_custom_diag (bs, move (diag.first), diag.second);
    }

    if (exec_body)
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

      if (script.body_temp_dir            &&
          !script.depdb_preamble_temp_dir &&
          !script.diag_preamble_temp_dir)
        env.set_temp_dir_variable ();

      p.execute_body (rs, bs,
                      env, script, run,
                      !exec_depdb && !exec_diag /* enter */,
                      false                     /* leave */);

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
    }

    return exec_diag || exec_body;
  }

  bool adhoc_buildscript_rule::
  execute_update_group (const scope& bs,
                        action a, const group& g,
                        build::script::environment& env,
                        build::script::default_runner& run,
                        bool deferred_failure) const
  {
    // Note: similar to execute_update_file() above (see there for comments).
    //
    // NOTE: when called from perform_update_file_or_group_dyndep_byproduct(),
    //       the group does not contain dynamic members yet and thus could
    //       have no members at all.
    //
    context& ctx (g.ctx);

    const scope& rs (*bs.root_scope ());

    build::script::parser p (ctx);

    bool exec_body  (!ctx.dry_run || verb >= 2);
    bool exec_diag  (!script.diag_preamble.empty () && (exec_body || verb == 1));
    bool exec_depdb (!script.depdb_preamble.empty ());

    if (script.diag_name)
    {
      if (verb == 1)
      {
        const file* pt (nullptr);
        for (const prerequisite_target& p: g.prerequisite_targets[a])
        {
          if (p.target != nullptr && !p.adhoc ())
          {
            pt = p.target->is_a<file> ();
            break;
          }
        }

        if (pt != nullptr)
          print_diag (script.diag_name->c_str (), *pt, g);
        else
          print_diag (script.diag_name->c_str (), g);
      }
    }
    else if (exec_diag)
    {
      if (script.diag_preamble_temp_dir && !script.depdb_preamble_temp_dir)
        env.set_temp_dir_variable ();

      pair<names, location> diag (
        p.execute_diag_preamble (rs, bs,
                                 env, script, run,
                                 verb == 1   /* diag */,
                                 !exec_depdb /* enter */,
                                 false       /* leave */));
      if (verb == 1)
        print_custom_diag (bs, move (diag.first), diag.second);
    }

    if (exec_body)
    {
      // On failure remove the target files that may potentially exist but
      // be invalid.
      //
      // Note: we may leave dynamic members if we don't know about them yet.
      // Feels natural enough.
      //
      small_vector<auto_rmfile, 8> rms;

      if (!ctx.dry_run)
      {
        for (const target* m: g.members)
        {
          if (auto* f = m->is_a<file> ())
            rms.emplace_back (f->path ());
        }
      }

      if (script.body_temp_dir            &&
          !script.depdb_preamble_temp_dir &&
          !script.diag_preamble_temp_dir)
        env.set_temp_dir_variable ();

      p.execute_body (rs, bs,
                      env, script, run,
                      !exec_depdb && !exec_diag /* enter */,
                      false                     /* leave */);

      if (!ctx.dry_run)
      {
        if (deferred_failure)
          fail << "expected error exit status from recipe body";

        // @@ TODO: expl: byproduct
        //
        // Note: will not work for dynamic members if we don't know about them
        // yet. Could probably fix by doing this later, after the dynamic
        // dependency extraction.
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

        for (const target* m: g.members)
        {
          if (auto* p = m->is_a<exe> ())
            chmod (p->path ());
        }
#endif
        for (auto& rm: rms)
          rm.cancel ();
      }
    }

    return exec_diag || exec_body;
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
    // Finally, we print the entire ad hoc group at verbosity level 1, similar
    // to the default update diagnostics.
    //
    // @@ TODO: .t may also be a temporary directory (and below).
    //
    return perform_clean_extra (a,
                                t.as<file> (),
                                {".d", ".t"},
                                {},
                                true /* show_adhoc_members */);
  }

  target_state adhoc_buildscript_rule::
  perform_clean_group (action a, const target& xt)
  {
    const group& g (xt.as<group> ());

    path d, t;
    if (g.members_static != 0)
    {
      const path& p (g.members.front ()->as<file> ().path ());
      d = p + ".d";
      t = p + ".t";
    }
    else
    {
      // See target_path lambda in apply().
      //
      t = g.dir / (g.name + '.' + g.type ().name);
      d = t + ".d";
      t += ".t";
    }

    return perform_clean_group_extra (a, g, {d.string ().c_str (),
                                             t.string ().c_str ()});
  }

  target_state adhoc_buildscript_rule::
  default_action (action a,
                  const target& t,
                  const optional<timestamp>& deadline) const
  {
    tracer trace ("adhoc_buildscript_rule::default_action");

    context& ctx (t.ctx);

    target_state ts (target_state::unchanged);

    if (ctx.current_mode == execution_mode::first)
      ts |= straight_execute_prerequisites (a, t);

    bool exec (!ctx.dry_run || verb != 0);

    // Special handling for fsdir{} (which is the recommended if somewhat
    // hackish way to represent directory symlinks). See fsdir_rule for
    // background.
    //
    // @@ Note that because there is no depdb, we cannot detect the target
    //    directory change (or any other changes in the script).
    //
    if (exec                                              &&
        (a == perform_update_id || a == perform_clean_id) &&
        t.is_a<fsdir> ())
    {
      // For update we only want to skip if it's a directory. For clean we
      // want to (try) to clean up any filesystem entry, including a dangling
      // symlink.
      //
      exec = a == perform_update_id
        ? !exists (t.dir, true /* ignore_errors */)
        : build2::entry_exists (t.dir, false /* follow_symlinks */);
    }

    if (exec)
    {
      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      build::script::environment e (a, t, bs, false /* temp_dir */, deadline);
      build::script::parser p (ctx);
      build::script::default_runner r;

      bool exec_body (!ctx.dry_run || verb >= 2);
      bool exec_diag (!script.diag_preamble.empty () &&
                      (exec_body || verb == 1));

      if (script.diag_name)
      {
        if (verb == 1)
        {
          // For operations other than update (as well as for non-file
          // targets), we default to the second form (without the
          // prerequisite). Think test.
          //
          if (t.adhoc_member == nullptr)
            print_diag (script.diag_name->c_str (), t);
          else
          {
            vector<target_key> ts;
            for (const target* m (&t); m != nullptr; m = m->adhoc_member)
              ts.push_back (m->key ());

            print_diag (script.diag_name->c_str (), move (ts));
          }
        }
      }
      else if (exec_diag)
      {
        if (script.diag_preamble_temp_dir)
          e.set_temp_dir_variable ();

        pair<names, location> diag (
          p.execute_diag_preamble (rs, bs,
                                   e, script, r,
                                   verb == 1  /* diag */,
                                   true       /* enter */,
                                   !exec_body /* leave */));

        if (verb == 1)
          print_custom_diag (bs, move (diag.first), diag.second);
      }

      if (exec_body)
      {
        if (script.body_temp_dir && !script.diag_preamble_temp_dir)
          e.set_temp_dir_variable ();

        p.execute_body (rs, bs, e, script, r, !exec_diag /* enter */);
      }

      ts |= target_state::changed;
    }

    if (ctx.current_mode == execution_mode::last)
      ts |= reverse_execute_prerequisites (a, t);

    return ts;
  }

  void adhoc_buildscript_rule::
  print_custom_diag (const scope& bs, names&& ns, const location& l) const
  {
    // The straightforward thing to do would be to just print the diagnostics
    // as specified by the user. But that will make some of the tidying up
    // done by print_diag() unavailable to custom diagnostics. Things like
    // omitting the out-qualification as well as compact printing of the
    // groups. Also, in the future we may want to support colorization of the
    // diagnostics, which will be difficult to achive with such a "just print"
    // approach.
    //
    // So instead we are going to parse the custom diagnostics, translate
    // names back to targets (where appropriate), and call one of the
    // print_diag() functions. Specifically, we expect the custom diagnostics
    // to be in one of the following two forms (which correspond to the two
    // forms of pring_diag()):
    //
    // diag <prog> <l-target> <comb> <r-target>...
    // diag <prog> <r-target>...
    //
    // And the way we are going to disambiguate this is by analyzing name
    // types. Specifically, we expect <comb> to be a simple name that also
    // does not contain any directory separators (so we can distinguish it
    // from both target names as well as paths, which can be specified on
    // either side).  We will also recognize `-` as the special stdout path
    // name (so <comb> cannot be `-`). Finally, <l-target> (but not
    // <r-target>) can be a string (e.g., an argument) but that should not
    // pose an ambiguity.
    //
    // With this approach, the way to re-create the default diagnostics would
    // be:
    //
    // diag <prog> ($<[0]) -> $>
    // diag <prog> $>
    //
    auto i (ns.begin ()), e (ns.end ());

    // <prog>
    //
    if (i == e)
      fail (l) << "missing program name in diag builtin";

    if (!i->simple () || i->empty ())
      fail (l) << "expected simple name as program name in diag builtin";

    const char* prog (i->value.c_str ());
    ++i;

    // <l-target>
    //
    const target* l_t (nullptr);
    path l_p;
    string l_s;

    auto parse_target = [&bs, &l, &i, &e] () -> const target&
    {
      name& n (*i++);
      name  o;

      if (n.pair)
      {
        if (i == e)
          fail (l) << "invalid target name pair in diag builtin";

        o = move (*i++);
      }

      // Similar to to_target() in $target.*().
      //
      if (const target* r = search_existing (n, bs, o.dir))
        return *r;

      fail (l) << "target "
               << (n.pair ? names {move (n), move (o)} : names {move (n)})
               << " not found in diag builtin" << endf;
    };

    auto parse_first = [&l, &i, &e,
                        &parse_target] (const target*& t, path& p, string& s,
                                        const char* after)
    {
      if (i == e)
        fail (l) << "missing target after " << after << " in diag builtin";

      try
      {
        if (i->typed ())
        {
          t = &parse_target ();
          return; // i is already incremented.
        }
        else if (!i->dir.empty ())
        {
          p = move (i->dir);
          p /= i->value;
        }
        else if (path_traits::find_separator (i->value) != string::npos)
        {
          p = path (move (i->value));
        }
        else if (!i->value.empty ())
        {
          s = move (i->value);
        }
        else
          fail (l) << "expected target, path, or argument after "
                   << after << " in diag builtin";
      }
      catch (const invalid_path& e)
      {
        fail (l) << "invalid path '" << e.path <<  "' after "
                 << after << " in diag builtin";
      }

      ++i;
    };

    parse_first (l_t, l_p, l_s, "program name");

    // Now detect which form it is.
    //
    if (i != e &&
        i->simple () &&
        !i->empty () &&
        path_traits::find_separator (i->value) == string::npos)
    {
      // The first form.

      // <comb>
      //
      const char* comb (i->value.c_str ());
      ++i;

      // <r-target>
      //
      const target* r_t (nullptr);
      path r_p;
      string r_s;

      parse_first (r_t, r_p, r_s, "combiner");

      path_name r_pn;

      if (r_t != nullptr)
        ;
      else if (!r_p.empty ())
        r_pn = path_name (&r_p);
      else
      {
        if (r_s != "-")
          fail (l) << "expected target or path instead of '" << r_s
                   << "' after combiner in diag builtin";

        r_pn = path_name (move (r_s));
      }

      if (i == e)
      {
        if (r_t != nullptr)
        {
          if      (l_t != nullptr) print_diag (prog, *l_t, *r_t, comb);
          else if (!l_p.empty ())  print_diag (prog,  l_p, *r_t, comb);
          else                     print_diag (prog,  l_s, *r_t, comb);
        }
        else
        {
          if      (l_t != nullptr) print_diag (prog, *l_t, r_pn, comb);
          else if (!l_p.empty ())  print_diag (prog,  l_p, r_pn, comb);
          else                     print_diag (prog,  l_s, r_pn, comb);
        }

        return;
      }

      // We can only have multiple targets, not paths.
      //
      if (r_t == nullptr)
        fail (l) << "unexpected name after path in diag builtin";

      // <r-target>...
      //
      vector<target_key> r_ts {r_t->key ()};

      do r_ts.push_back (parse_target ().key ()); while (i != e);

      if      (l_t != nullptr) print_diag (prog,  l_t->key (), move (r_ts), comb);
      else if (!l_p.empty ())  print_diag (prog,  l_p,         move (r_ts), comb);
      else                     print_diag (prog,  l_s,         move (r_ts), comb);
    }
    else
    {
      // The second form.

      // First "absorb" the l_* values as the first <r-target>.
      //
      const target* r_t (nullptr);
      path_name r_pn;

      if (l_t != nullptr)
        r_t = l_t;
      else if (!l_p.empty ())
        r_pn = path_name (&l_p);
      else
      {
        if (l_s != "-")
        {
          diag_record dr (fail (l));

          dr << "expected target or path instead of '" << l_s
             << "' after program name in diag builtin";

          if (i != e)
            dr << info << "alternatively, missing combiner after '"
               << l_s << "'";
        }

        r_pn = path_name (move (l_s));
      }

      if (i == e)
      {
        if (r_t != nullptr)
          print_diag (prog, *r_t);
        else
          print_diag (prog, r_pn);

        return;
      }

      // We can only have multiple targets, not paths.
      //
      if (r_t == nullptr)
        fail (l) << "unexpected name after path in diag builtin";

      // <r-target>...
      //
      vector<target_key> r_ts {r_t->key ()};

      do r_ts.push_back (parse_target ().key ()); while (i != e);

      print_diag (prog, move (r_ts));
    }
  }
}
