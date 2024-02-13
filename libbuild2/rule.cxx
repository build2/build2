// file      : libbuild2/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/rule.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // rule
  //
  rule::
  ~rule ()
  {
  }

  void rule::
  apply_posthoc (action, target&, match_extra&) const
  {
  }

  void rule::
  reapply (action, target&, match_extra&) const
  {
    // Unless the rule overrode cur_options, this function should never get
    // called. And if it did, then it should override this function.
    //
    assert (false);
  }

  const target* rule::
  import (const prerequisite_key&,
          const optional<string>&,
          const location&) const
  {
    return nullptr;
  }

  const rule_match*
  match_adhoc_recipe (action, target&, match_extra&); // algorithm.cxx

  bool rule::
  sub_match (const string& n, operation_id o,
             action a, target& t, match_extra& me) const
  {
    // First check for an ad hoc recipe (see match_rule_impl() for details).
    //
    if (!t.adhoc_recipes.empty ())
    {
      // Use scratch match_extra since if there is no recipe, then we don't
      // want to keep any changes and if there is, then we want it discarded.
      //
      match_extra s (true /* locked */); // Not called from adhoc_rule::match().
      if (match_adhoc_recipe (action (a.meta_operation (), o), t, s) != nullptr)
        return false;
    }

    const string& h (t.find_hint (o));
    return name_rule_map::sub (h, n) && match (a, t, h, me);
  }

  // simple_rule
  //
  bool simple_rule::
  match (action a, target& t, const string&, match_extra&) const
  {
    return match (a, t);
  }

  recipe simple_rule::
  apply (action a, target& t, match_extra&) const
  {
    return apply (a, t);
  }

  bool simple_rule::
  sub_match (const string& n, operation_id o,
             action a, target& t) const
  {
    if (!t.adhoc_recipes.empty ())
    {
      match_extra s (true /* locked */); // Not called from adhoc_rule::match().
      if (match_adhoc_recipe (action (a.meta_operation (), o), t, s) != nullptr)
        return false;
    }

    return name_rule_map::sub (t.find_hint (o), n) && match (a, t);
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
  match (action a, target& t, const string&, match_extra&) const
  {
    tracer trace ("file_rule::match");

    if (match_type_ && !t.is_a<mtime_target> ())
      return false;

    // While strictly speaking we should check for the file's existence
    // for every action (because that's the condition for us matching),
    // for some actions this is clearly a waste. Say, perform_clean: we
    // are not doing anything for this action so not checking if the file
    // exists seems harmless.
    //
    // But we also don't want to match real targets and not cleaning their
    // output files.
    //
    switch (a)
    {
    case perform_clean_id:
      return t.decl != target_decl::real;
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
        // See also path_target::path_mtime() for a potential race in this
        // logic.
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
          if (const string* e = pt->derive_extension (true))
          {
            p = &pt->derive_path_with_extension (*e);
          }
          else
          {
            l4 ([&]{trace << "no default extension for target " << *pt;});
            return false;
          }
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
  apply (action a, target& t, match_extra&) const
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
  const rule_match file_rule::rule_match ("build.file", file_rule::instance);

  // alias_rule
  //
  bool alias_rule::
  match (action, target&) const
  {
    return true;
  }

  recipe alias_rule::
  apply (action a, target& t) const
  {
    // Inject dependency on our directory (note: not parent) so that it is
    // automatically created on update and removed on clean.
    //
    inject_fsdir (a, t, true, true, false);

    // Handle the alias match-only level.
    //
    match_search ms;
    if (t.ctx.match_only && *t.ctx.match_only == match_only_level::alias)
    {
      ms = [] (action,
               const target& t,
               const prerequisite& p,
               include_type i)
        {
          return prerequisite_target (
            p.is_a<alias> () ? &search (t, p) : nullptr,
            i);
        };
    }

    match_prerequisites (a, t, ms);
    return default_recipe;
  }

  const alias_rule alias_rule::instance;

  // fsdir_rule
  //
  bool fsdir_rule::
  match (action, target&) const
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
        print_diag ("mkdir", t);
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
  perform_update_direct (action a, const fsdir& t)
  {
    assert (t.ctx.phase == run_phase::match);

    // First create the parent directory. If present, it is always first.
    //
    if (const target* p = (t.prerequisite_targets[a].empty ()
                           ? nullptr
                           : t.prerequisite_targets[a][0]))
    {
      if (const fsdir* fp = p->is_a<fsdir> ())
        perform_update_direct (a, *fp);
    }

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

    // The same code as in perform_clean_direct() below.
    //
    target_state ts (rmdir (t.dir, t, t.ctx.current_diag_noise ? 1 : 2)
                     ? target_state::changed
                     : target_state::unchanged);

    if (!t.prerequisite_targets[a].empty ())
      ts |= reverse_execute_prerequisites (a, t);

    return ts;
  }

  void fsdir_rule::
  perform_clean_direct (action a, const fsdir& t)
  {
    assert (t.ctx.phase == run_phase::match);

    // The same code as in perform_clean() above.
    //
    // Except that if there are other dependens of this fsdir{} then this will
    // likely be a noop (because the directory won't be empty) and it makes
    // sense to just defer cleaning to such other dependents. See
    // clean_during_match() for backgound. This is similar logic as in
    // unmatch::safe.
    //
    if (t[a].dependents.load (memory_order_relaxed) == 0)
    {
      rmdir (t.dir, t, t.ctx.current_diag_noise ? 1 : 2);

      // Then clean the parent directory. If present, it is always first.
      //
      if (const target* p = (t.prerequisite_targets[a].empty ()
                             ? nullptr
                             : t.prerequisite_targets[a][0]))
      {
        if (const fsdir* fp = p->is_a<fsdir> ())
          perform_clean_direct (a, *fp);
      }
    }
  }

  const fsdir_rule fsdir_rule::instance;

  // noop_rule
  //
  bool noop_rule::
  match (action, target&) const
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
  const dir_path adhoc_rule::recipes_build_dir ("recipes");

  bool adhoc_rule::
  reverse_fallback (action, const target_type&) const
  {
    return false;
  }

  bool adhoc_rule::
  match (action a, target& xt, const string& h, match_extra& me) const
  {
    const target& t (xt);
    return pattern == nullptr || pattern->match (a, t, h, me);
  }

  void adhoc_rule::
  dump_attributes (ostream&) const
  {
  }

  // adhoc_rule_with_deadline (vtable)
  //
  adhoc_rule_with_deadline::
  ~adhoc_rule_with_deadline ()
  {
  }

  // Scope operation callback that cleans up recipe builds.
  //
  target_state adhoc_rule::
  clean_recipes_build (action, const scope& rs, const dir&)
  {
    context& ctx (rs.ctx);

    const dir_path& out_root (rs.out_path ());

    dir_path d (out_root / rs.root_extra->build_build_dir / recipes_build_dir);

    if (exists (d))
    {
      if (rmdir_r (ctx, d))
      {
        // Clean up build/build/ if it also became empty.
        //
        d = out_root / rs.root_extra->build_build_dir;
        if (empty (d))
        {
          rmdir (ctx, d, 2);

          // Clean up build/ if it also became empty (e.g., in case of a build
          // with a transient configuration).
          //
          d = out_root / rs.root_extra->build_dir;
          if (empty (d))
            rmdir (ctx, d, 2);
        }

        return target_state::changed;
      }
    }

    return target_state::unchanged;
  }

  // adhoc_rule_pattern (vtable)
  //
  adhoc_rule_pattern::
  ~adhoc_rule_pattern ()
  {
  }

  bool adhoc_rule_pattern::fallback_rule::
  match (action, target&, const string&, match_extra&) const
  {
    return false;
  }

  recipe adhoc_rule_pattern::fallback_rule::
  apply (action, target&, match_extra&) const
  {
    return empty_recipe;
  }
}
