// file      : libbuild2/install/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/rule.hxx>
#include <libbuild2/install/utility.hxx> // resolve_dir() declaration

#include <libbutl/filesystem.hxx> // dir_exists(), file_exists()

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/install/operation.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace install
  {
    // Lookup the install or install.* variable. Return NULL if not found or
    // if the value is the special 'false' name (which means do not install;
    // so the result can be used as bool). T is either scope or target.
    //
    template <typename P, typename T>
    static const P*
    lookup_install (T& t, const string& var)
    {
      auto l (t[var]);

      if (!l)
        return nullptr;

      const P& r (cast<P> (l));
      return r.simple () && r.string () == "false" ? nullptr : &r;
    }

    // Note that the below rules are called for both install and
    // update-for-install.
    //
    // @@ TODO: we clearly need a module class.
    //
    static inline const variable&
    var_install (const scope& rs)
    {
      context& ctx (rs.ctx);

      return *rs.root_extra->operations[
        (ctx.current_outer_oif != nullptr
         ? ctx.current_outer_oif
         : ctx.current_inner_oif)->id].ovar;
    }

    // alias_rule
    //
    const alias_rule alias_rule::instance;

    bool alias_rule::
    match (action, target&) const
    {
      // We always match.
      //
      // Note that we are called both as the outer part during the update-for-
      // un/install pre-operation and as the inner part during the un/install
      // operation itself.
      //
      return true;
    }

    pair<const target*, uint64_t> alias_rule::
    filter (const scope* is,
            action a, const target& t, prerequisite_iterator& i,
            match_extra& me) const
    {
      assert (i->member == nullptr);
      return filter (is, a, t, i->prerequisite, me);
    }

    pair<const target*, uint64_t> alias_rule::
    filter (const scope* is,
            action, const target& t, const prerequisite& p,
            match_extra&) const
    {
      const target& pt (search (t, p));
      const uint64_t options (match_extra::all_options); // No definition.
      return make_pair (is == nullptr || pt.in (*is) ? &pt : nullptr, options);
    }

    recipe alias_rule::
    apply (action a, target& t, match_extra& me) const
    {
      return apply_impl (a, t, me);
    }

    recipe alias_rule::
    apply (action, target&) const
    {
      assert (false); // Never called.
      return nullptr;
    }

    recipe alias_rule::
    apply_impl (action a, target& t, match_extra& me, bool reapply) const
    {
      tracer trace ("install::alias_rule::apply");

      assert (!reapply || a.operation () != update_id);

      // Pass-through to our installable prerequisites.
      //
      // @@ Shouldn't we do match in parallel (here and below)?
      //
      optional<const scope*> is; // Installation scope (resolve lazily).

      auto& pts (t.prerequisite_targets[a]);
      auto pms (group_prerequisite_members (a, t, members_mode::never));
      for (auto i (pms.begin ()), e (pms.end ()); i != e; ++i)
      {
        // NOTE: see essentially the same logic in reapply_impl() below.
        //
        const prerequisite& p (i->prerequisite);

        // Ignore excluded.
        //
        include_type pi (include (a, t, p));

        if (!pi)
          continue;

        // Ignore unresolved targets that are imported from other projects.
        // We are definitely not installing those.
        //
        if (p.proj)
          continue;

        // Let a customized rule have its say.
        //
        // Note: we assume that if the filter enters the group, then it
        // iterates over all its members.
        //
        if (!is)
          is = a.operation () != update_id ? install_scope (t) : nullptr;

        pair<const target*, uint64_t> fr (filter (*is, a, t, i, me));

        const target* pt (fr.first);
        uint64_t options (fr.second);

        lookup l;

        if (pt == nullptr)
        {
          l5 ([&]{trace << "ignoring " << p << " (filtered out)";});
        }
        // Check if this prerequisite is explicitly "not installable", that
        // is, there is the 'install' variable and its value is false.
        //
        // At first, this might seem redundand since we could have let the
        // file_rule below take care of it. The nuance is this: this
        // prerequsite can be in a different subproject that hasn't loaded the
        // install module (and therefore has no file_rule registered). The
        // typical example would be the 'tests' subproject.
        //
        // Note: not the same as lookup_install() above.
        //
        else if ((l = (*pt)[var_install (*p.scope.root_scope ())]) &&
                 cast<path> (l).string () == "false")
        {
          l5 ([&]{trace << "ignoring " << *pt << " (not installable)";});
          pt = nullptr;
        }
        // If this is not a file-based target (e.g., a target group such as
        // libu{}) then ignore it if there is no rule to install.
        //
        else if (pt->is_a<file> ())
        {
          match_sync (a, *pt, options);
        }
        else if (!try_match_sync (a, *pt, options).first)
        {
          l5 ([&]{trace << "ignoring " << *pt << " (no rule)";});
          pt = nullptr;
        }

        if (pt != nullptr || reapply)
        {
          // Use auxiliary data for a NULL entry to distinguish between
          // filtered out (1) and ignored for other reasons (0).
          //
          pts.push_back (
            prerequisite_target (pt, pi, fr.first == nullptr ? 1 : 0));
        }
      }

      return default_recipe;
    }

    void alias_rule::
    reapply_impl (action a, target& t, match_extra& me) const
    {
      tracer trace ("install::alias_rule::reapply");

      assert (a.operation () != update_id);

      optional<const scope*> is;

      // Iterate over prerequisites and prerequisite targets in parallel.
      //
      auto& pts (t.prerequisite_targets[a]);
      size_t j (0), n (pts.size ()), en (0);

      auto pms (group_prerequisite_members (a, t, members_mode::never));
      for (auto i (pms.begin ()), e (pms.end ());
           i != e && j != n;
           ++i, ++j, ++en)
      {
        // The same logic as in apply() above except that we skip
        // prerequisites that were not filtered out.
        //
        const prerequisite& p (i->prerequisite);

        include_type pi (include (a, t, p));
        if (!pi)
          continue;

        if (p.proj)
          continue;

        prerequisite_target& pto (pts[j]);

        if (pto.target != nullptr || pto.data == 0)
          continue;

        if (!is)
          is = a.operation () != update_id ? install_scope (t) : nullptr;

        pair<const target*, uint64_t> fr (filter (*is, a, t, i, me));

        const target* pt (fr.first);
        uint64_t options (fr.second);

        lookup l;

        if (pt == nullptr)
        {
          l5 ([&]{trace << "ignoring " << p << " (filtered out)";});
        }
        else if ((l = (*pt)[var_install (*p.scope.root_scope ())]) &&
                 cast<path> (l).string () == "false")
        {
          l5 ([&]{trace << "ignoring " << *pt << " (not installable)";});
          pt = nullptr;
        }
        else if (pt->is_a<file> ())
        {
          match_sync (a, *pt, options);
        }
        else if (!try_match_sync (a, *pt, options).first)
        {
          l5 ([&]{trace << "ignoring " << *pt << " (no rule)";});
          pt = nullptr;
        }

        pto = prerequisite_target (pt, pi, fr.first == nullptr ? 1 : 0);
      }

      assert (en == n); // Did not call apply() with true for reapply?
    }

    // group_rule
    //
    const group_rule group_rule::instance (false /* see_through_only */);

    bool group_rule::
    match (action a, target& t) const
    {
      return (!see_through_only || t.type ().see_through ()) &&
        alias_rule::match (a, t);
    }

    bool group_rule::
    filter (action, const target&, const target&) const
    {
      return true;
    }

    pair<const target*, uint64_t> group_rule::
    filter (const scope* is,
            action, const target& t, const prerequisite& p,
            match_extra&) const
    {
      const uint64_t options (match_extra::all_options); // No definition.
      pair<const target*, uint64_t> r (nullptr, options);

      // The same logic as in file_rule::filter() below.
      //
      if (p.is_a<exe> ())
      {
        const scope& rs (*p.scope.root_scope ());

        if (p.vars.empty () ||
            cast_empty<path> (p.vars[var_install (rs)]).string () != "true")
          return r;
      }

      const target& pt (search (t, p));
      if (is == nullptr || pt.in (*is))
        r.first = &pt;

      return r;
    }

    recipe group_rule::
    apply (action a, target& t, match_extra& me) const
    {
      tracer trace ("install::group_rule::apply");

      // Resolve group members.
      //
      // Remember that we are called twice: first during update for install
      // (pre-operation) and then during install. During the former, we rely
      // on the normal update rule to resolve the group members. During the
      // latter, there will be no rule to do this but the group will already
      // have been resolved by the pre-operation.
      //
      // If the rule could not resolve the group, then we ignore it.
      //
      group_view gv (a.outer ()
                     ? resolve_members (a, t)
                     : t.group_members (a));

      if (gv.members != nullptr && gv.count != 0)
      {
        const scope& rs (t.root_scope ());

        auto& pts (t.prerequisite_targets[a]);
        for (size_t i (0); i != gv.count; ++i)
        {
          const target* mt (gv.members[i]);

          if (mt == nullptr)
            continue;

          // Let a customized rule have its say.
          //
          if (!filter (a, t, *mt))
          {
            l5 ([&]{trace << "ignoring " << *mt << " (filtered out)";});
            continue;
          }

          // See if we were explicitly instructed not to touch this target
          // (the same semantics as in the prerequisites match).
          //
          // Note: not the same as lookup_install() above.
          //
          auto l ((*mt)[var_install (rs)]);
          if (l && cast<path> (l).string () == "false")
          {
            l5 ([&]{trace << "ignoring " << *mt << " (not installable)";});
            continue;
          }

          match_sync (a, *mt);
          pts.push_back (mt); // Never ad hoc.
        }
      }

      // Delegate to the base rule.
      //
      return alias_rule::apply (a, t, me);
    }


    // file_rule
    //
    const file_rule file_rule::instance;

    bool file_rule::
    match (action, target&) const
    {
      // We always match, even if this target is not installable (so that we
      // can ignore it; see apply()).
      //
      return true;
    }

    bool file_rule::
    filter (action, const target&, const target&) const
    {
      return true;
    }

    pair<const target*, uint64_t> file_rule::
    filter (const scope* is,
            action a, const target& t, prerequisite_iterator& i,
            match_extra& me) const
    {
      assert (i->member == nullptr);
      return filter (is, a, t, i->prerequisite, me);
    }

    pair<const target*, uint64_t> file_rule::
    filter (const scope* is,
            action, const target& t, const prerequisite& p,
            match_extra&) const
    {
      const uint64_t options (match_extra::all_options); // No definition.
      pair<const target*, uint64_t> r (nullptr, options);

      // See also group_rule::filter() with identical semantics.
      //
      if (p.is_a<exe> ())
      {
        const scope& rs (*p.scope.root_scope ());

        // Note that while include() checks for install=false, here we need to
        // check for explicit install=true. We could have re-used the lookup
        // performed by include(), but then we would have had to drag it
        // through and also diagnose any invalid values.
        //
        if (p.vars.empty () ||
            cast_empty<path> (p.vars[var_install (rs)]).string () != "true")
          return r;
      }

      const target& pt (search (t, p));
      if (is == nullptr || pt.in (*is))
        r.first = &pt;

      return r;
    }

    recipe file_rule::
    apply (action a, target& t, match_extra& me) const
    {
      recipe r (apply_impl (a, t, me));
      return r != nullptr ? move (r) : noop_recipe;
    }

    recipe file_rule::
    apply (action, target&) const
    {
      assert (false); // Never called.
      return nullptr;
    }

    recipe file_rule::
    apply_impl (action a, target& t, match_extra& me, bool reapply) const
    {
      tracer trace ("install::file_rule::apply");

      assert (!reapply || a.operation () != update_id);

      // Note that we are called both as the outer part during the update-for-
      // un/install pre-operation and as the inner part during the un/install
      // operation itself.
      //
      // In both cases we first determine if the target is installable and
      // return noop if it's not. Otherwise, in the first case (update-for-
      // un/install) we delegate to the normal update and in the second
      // (un/install) -- perform the install.
      //
      if (!lookup_install<path> (t, "install"))
        return empty_recipe;

      // In both cases, the next step is to search, match, and collect all the
      // installable prerequisites.
      //
      // But first, in case of the update pre-operation, match the inner rule
      // (actual update). We used to do this after matching the prerequisites
      // but the inner rule may provide some rule-specific information (like
      // the target extension for exe{}) that may be required during the
      // prerequisite search (like the base name for in{}; this no longer
      // reproduces likely due to the changes to exe{} extension derivation
      // but a contrived arrangement can still be made to trigger this).
      //
      // But then we discovered that doing this before the prerequisites messes
      // up with the for-install signaling. Specifically, matching the
      // prerequisites may signal that they are being updated for install,
      // for example, for a library via a metadata library used in a moc
      // recipe. While matching the inner rule may trigger updating during
      // match of such prerequisites, for example, a source file generated by
      // that moc recipe that depends on this metadata library. If we match
      // prerequisites before, then the library that is pulled by the metadata
      // library will be updated before we had a chance to signal that it
      // should be updated for install.
      //
      // To try to accommodate both cases (as best as we can) we now split the
      // inner rule match into two steps: we do the match before and apply
      // after. This allows rules that deal with tricky prerequisites like
      // in{} to assign the target path in match() instead of apply() (see
      // in::rule, for example).
      //
#if 0
      optional<bool> unchanged;
      if (a.operation () == update_id)
        unchanged = match_inner (a, t, unmatch::unchanged).first;
#else
      action ia (a.inner_action ());
      if (a.operation () == update_id)
        match_only_sync (ia, t);
#endif

      optional<const scope*> is; // Installation scope (resolve lazily).

      auto& pts (t.prerequisite_targets[a]);
      auto pms (group_prerequisite_members (a, t, members_mode::never));
      for (auto i (pms.begin ()), e (pms.end ()); i != e; ++i)
      {
        // NOTE: see essentially the same logic in reapply_impl() below.
        //
        const prerequisite& p (i->prerequisite);

        // Ignore excluded.
        //
        include_type pi (include (a, t, p));

        if (!pi)
          continue;

        // Ignore unresolved targets that are imported from other projects.
        // We are definitely not installing those.
        //
        if (p.proj)
          continue;

        // Let a customized rule have its say.
        //
        // Note: we assume that if the filter enters the group, then it
        // iterates over all its members.
        //
        if (!is)
          is = a.operation () != update_id ? install_scope (t) : nullptr;

        pair<const target*, uint64_t> fr (filter (*is, a, t, i, me));

        const target* pt (fr.first);
        uint64_t options (fr.second);

        lookup l;

        if (pt == nullptr)
        {
          l5 ([&]{trace << "ignoring " << p << " (filtered out)";});
        }
        //
        // See if we were explicitly instructed not to touch this target (the
        // same semantics as in alias_rule).
        //
        // Note: not the same as lookup_install() above.
        //
        else if ((l = (*pt)[var_install (*p.scope.root_scope ())]) &&
                 cast<path> (l).string () == "false")
        {
          l5 ([&]{trace << "ignoring " << *pt << " (not installable)";});
          pt = nullptr;
        }
        else if (pt->is_a<file> ())
        {
          // If the matched rule returned noop_recipe, then the target state
          // is set to unchanged as an optimization. Use this knowledge to
          // optimize things on our side as well since this will help a lot
          // when updating static installable content (headers, documentation,
          // etc).
          //
          // Regarding options, the expectation here is that they are not used
          // for the update operation. And for install/uninstall, if they are
          // used, then they don't effect whether the target is unchanged. All
          // feels reasonable.
          //
          if (match_sync (a, *pt, unmatch::unchanged, options).first)
            pt = nullptr;
        }
        else if (!try_match_sync (a, *pt, options).first)
        {
          l5 ([&]{trace << "ignoring " << *pt << " (no rule)";});
          pt = nullptr;
        }

        if (pt != nullptr || reapply)
        {
          // Use auxiliary data for a NULL entry to distinguish between
          // filtered out (1) and ignored for other reasons (0).
          //
          pts.push_back (
            prerequisite_target (pt, pi, fr.first == nullptr ? 1 : 0));
        }
      }

#if 1
      optional<bool> unchanged;
      if (a.operation () == update_id)
        unchanged = match_sync (ia, t, unmatch::unchanged).first;
#endif

      if (a.operation () == update_id)
      {
        return *unchanged
          ? (pts.empty () ? noop_recipe : default_recipe)
          : &perform_update;
      }
      else
      {
        return [this] (action a, const target& t)
        {
          return a.operation () == install_id
            ? perform_install   (a, t)
            : perform_uninstall (a, t);
        };
      }
    }

    void file_rule::
    reapply_impl (action a, target& t, match_extra& me) const
    {
      tracer trace ("install::file_rule::reapply");

      assert (a.operation () != update_id);

      optional<const scope*> is;

      // Iterate over prerequisites and prerequisite targets in parallel.
      //
      auto& pts (t.prerequisite_targets[a]);
      size_t j (0), n (pts.size ()), en (0);

      auto pms (group_prerequisite_members (a, t, members_mode::never));
      for (auto i (pms.begin ()), e (pms.end ());
           i != e && j != n;
           ++i, ++j, ++en)
      {
        // The same logic as in apply() above except that we skip
        // prerequisites that were not filtered out.
        //
        const prerequisite& p (i->prerequisite);

        include_type pi (include (a, t, p));
        if (!pi)
          continue;

        if (p.proj)
          continue;

        prerequisite_target& pto (pts[j]);

        if (pto.target != nullptr || pto.data == 0)
          continue;

        if (!is)
          is = a.operation () != update_id ? install_scope (t) : nullptr;

        pair<const target*, uint64_t> fr (filter (*is, a, t, i, me));

        const target* pt (fr.first);
        uint64_t options (fr.second);

        lookup l;

        if (pt == nullptr)
        {
          l5 ([&]{trace << "ignoring " << p << " (filtered out)";});
        }
        else if ((l = (*pt)[var_install (*p.scope.root_scope ())]) &&
                 cast<path> (l).string () == "false")
        {
          l5 ([&]{trace << "ignoring " << *pt << " (not installable)";});
          pt = nullptr;
        }
        else if (pt->is_a<file> ())
        {
          if (match_sync (a, *pt, unmatch::unchanged, options).first)
            pt = nullptr;
        }
        else if (!try_match_sync (a, *pt, options).first)
        {
          l5 ([&]{trace << "ignoring " << *pt << " (no rule)";});
          pt = nullptr;
        }

        pto = prerequisite_target (pt, pi, fr.first == nullptr ? 1 : 0);
      }

      assert (en == n); // Did not call apply() with true for reapply?
    }

    target_state file_rule::
    perform_update (action a, const target& t)
    {
      // First execute the inner recipe then prerequisites.
      //
      target_state ts (execute_inner (a, t));

      if (t.prerequisite_targets[a].size () != 0)
        ts |= straight_execute_prerequisites (a, t);

      return ts;
    }

    bool file_rule::
    install_extra (const file&, const install_dir&) const
    {
      return false;
    }

    bool file_rule::
    uninstall_extra (const file&, const install_dir&) const
    {
      return false;
    }

    auto_rmfile file_rule::
    install_pre (const file& t, const install_dir&) const
    {
      return auto_rmfile (t.path (), false /* active */);
    }

    bool file_rule::
    install_post (const file& t, const install_dir& id, auto_rmfile&&) const
    {
      return install_extra (t, id);
    }

    struct install_dir
    {
      dir_path dir;

      // If not NULL, then point to the corresponding install.* value.
      //
      const string*  sudo     = nullptr;
      const path*    cmd      = nullptr;
      const strings* options  = nullptr;
      const string*  mode     = nullptr;
      const string*  dir_mode = nullptr;

      explicit
      install_dir (dir_path d = dir_path ()): dir (move (d)) {}

      install_dir (dir_path d, const install_dir& b)
          : dir (move (d)),
            sudo (b.sudo),
            cmd (b.cmd),
            options (b.options),
            mode (b.mode),
            dir_mode (b.dir_mode) {}
    };

    using install_dirs = vector<install_dir>;

    // Calculate a subdirectory based on l's location (*.subdirs) and if not
    // empty add it to install_dirs. Return the new last element.
    //
    static install_dir&
    resolve_subdir (install_dirs& rs,
                    const target& t,
                    const scope& s,
                    const lookup& l)
    {
      // Find the scope from which this value came and use as a base
      // to calculate the subdirectory.
      //
      for (const scope* p (&s); p != nullptr; p = p->parent_scope ())
      {
        if (l.belongs (*p, true)) // Include target type/pattern-specific.
        {
          // The target can be in out or src.
          //
          const dir_path& d (t.out_dir ().leaf (p->out_path ()));

          // Add it as another leading directory rather than modifying
          // the last one directly; somehow, it feels right. Note: the
          // result is normalized.
          //
          if (!d.empty ())
            rs.emplace_back (rs.back ().dir / d, rs.back ());
          break;
        }
      }

      return rs.back ();
    }

    // Resolve installation directory name to absolute and normalized
    // directory path. Return all the super-directories leading up to the
    // destination (last).
    //
    // If target is not NULL, then also handle the subdirs logic.
    //
    // @@ TODO: detect cycles (maybe by keeping a stack-based linked list).
    //
    static install_dirs
    resolve (const scope& s,
             const target* t,
             dir_path d,
             bool fail_unknown = true,
             const string* var = nullptr)
    {
      install_dirs rs;

      if (d.absolute ())
        rs.emplace_back (move (d.normalize ()));
      else
      {
        // If it is relative, then the first component is treated as the
        // installation directory name, e.g., bin, sbin, lib, etc. Look it
        // up and recurse.
        //
        if (d.empty ())
          fail << "empty installation directory name";

        const string& sn (*d.begin ());
        const string var ("install." + sn);
        if (const dir_path* dn = lookup_install<dir_path> (s, var))
        {
          if (dn->empty ())
            fail << "empty installation directory for name " << sn <<
              info << "did you specified empty config." << var << "?";

          rs = resolve (s, t, *dn, fail_unknown, &var);

          if (rs.empty ())
          {
            assert (!fail_unknown);
            return rs; // Empty.
          }

          d = rs.back ().dir / dir_path (++d.begin (), d.end ());
          rs.emplace_back (move (d.normalize ()), rs.back ());
        }
        else
        {
          if (fail_unknown)
            fail << "unknown installation directory name '" << sn << "'" <<
              info << "did you forget to specify config." << var << "?" <<
              info << "specify !config." << var << "=... if installing "
                   << "from multiple projects";

          return rs; // Empty.
        }
      }

      install_dir* r (&rs.back ());

      // Override components in install_dir if we have our own.
      //
      if (var != nullptr)
      {
        if (auto l = s[*var + ".sudo"])     r->sudo     = &cast<string> (l);
        if (auto l = s[*var + ".cmd"])      r->cmd      = &cast<path> (l);
        if (auto l = s[*var + ".mode"])     r->mode     = &cast<string> (l);
        if (auto l = s[*var + ".dir_mode"]) r->dir_mode = &cast<string> (l);
        if (auto l = s[*var + ".options"])  r->options  = &cast<strings> (l);

        if (t != nullptr)
        {
          if (auto l = s[*var + ".subdirs"])
          {
            if (cast<bool> (l))
              r = &resolve_subdir (rs, *t, s, l);
          }
        }
      }

      // Set globals for unspecified components.
      //
      if (r->sudo == nullptr)
        r->sudo = cast_null<string> (s["config.install.sudo"]);

      if (r->cmd == nullptr)
        r->cmd = &cast<path> (s["config.install.cmd"]);

      if (r->options == nullptr)
        r->options = cast_null<strings> (s["config.install.options"]);

      if (r->mode == nullptr)
        r->mode = &cast<string> (s["config.install.mode"]);

      if (r->dir_mode == nullptr)
        r->dir_mode = &cast<string> (s["config.install.dir_mode"]);

      return rs;
    }

    static dir_path
    resolve_dir (const scope& s, const target* t,
                 dir_path d, dir_path rb,
                 bool fail_unknown)
    {
      install_dirs rs (resolve (s, t, move (d), fail_unknown));

      if (rs.empty ())
        return dir_path ();

      dir_path r (move (rs.back ().dir));

      if (!rb.empty ())
      {
        dir_path b (resolve (s, t, move (rb), false).back ().dir);

        try
        {
          r = r.relative (b);
        }
        catch (const invalid_path&)
        {
          fail << "unable to make installation directory " << r
               << " relative to " << b;
        }
      }

      return r;
    }

    dir_path
    resolve_dir (const target& t, dir_path d, dir_path rb, bool fail_unknown)
    {
      return resolve_dir (t.base_scope (), &t, move (d), move (rb), fail_unknown);
    }

    dir_path
    resolve_dir (const scope& s, dir_path d, dir_path rb, bool fail_unknown)
    {
      return resolve_dir (s, nullptr, move (d), move (rb), fail_unknown);
    }

    static inline install_dirs
    resolve (const target& t, dir_path d, bool fail_unknown = true)
    {
      return resolve (t.base_scope (), &t, move (d), fail_unknown);
    }

    path
    resolve_file (const file& f)
    {
      // Note: similar logic to perform_install().
      //
      const path* p (lookup_install<path> (f, "install"));

      if (p == nullptr) // Not installable.
        return path ();

      bool n (!p->to_directory ());
      dir_path d (n ? p->directory () : path_cast<dir_path> (*p));

      if (n && d.empty ())
        fail << "relative installation file path '" << p
             << "' has no directory component";

      install_dirs ids (resolve (f, d));

      if (!n)
      {
        if (auto l = f["install.subdirs"])
        {
          if (cast<bool> (l))
            resolve_subdir (ids, f, f.base_scope (), l);
        }
      }

      return ids.back ().dir / (n ? p->leaf () : f.path ().leaf ());
    }

    // On Windows we use MSYS2 install.exe and MSYS2 by default ignores
    // filesystem permissions (noacl mount option). And this means, for
    // example, that .exe that we install won't be runnable by Windows (MSYS2
    // itself will still run them since it recognizes the file extension).
    //
    // NOTE: this is no longer the case and we now use noacl (and acl causes
    // other problems; see baseutils fstab for details).
    //
    // The way we work around this (at least in our distribution of the MSYS2
    // tools) is by changing the mount option for cygdrives (/c, /d, etc) to
    // acl. But that's not all: we also have to install via a path that "hits"
    // one of those mount points, c:\foo won't work, we have to use /c/foo.
    // So this function translates an absolute Windows path to its MSYS
    // representation.
    //
    // Note that we return the result as a string, not dir_path since path
    // starting with / are illegal on Windows. Also note that the result
    // doesn't have the trailing slash.
    //
    static string
    msys_path (const dir_path& d)
    {
      assert (d.absolute ());
      string s (d.representation ());

      // First replace ':' with the drive letter (so the path is no longer
      // absolute) but postpone setting the first character to / until we are
      // a string.
      //
      s[1] = lcase (s[0]);
      s = dir_path (move (s)).posix_string ();
      s[0] = '/';

      return s;
    }

    void file_rule::
    install_d (const scope& rs,
               const install_dir& base,
               const dir_path& d,
               const file& t,
               uint16_t verbosity)
    {
      assert (d.absolute ());

      context& ctx (rs.ctx);

      // Here is the problem: if this is a dry-run, then we will keep showing
      // the same directory creation commands over and over again (because we
      // don't actually create them). There are two alternative ways to solve
      // this: actually create the directories or simply don't show anything.
      // While we use the former approach during update (see mkdir() in
      // filesystem), here it feels like we really shouldn't be touching the
      // destination filesystem. Plus, not showing anything will be symmetric
      // with uninstall since the directories won't be empty (because we don't
      // actually uninstall any files).
      //
      // Note that this also means we won't have the directory entries in the
      // manifest created with dry-run. Probably not a big deal.
      //
      if (ctx.dry_run || !filter_entry (rs, d, path (), entry_type::directory))
        return;

      dir_path chd (chroot_path (rs, d));

      try
      {
        if (dir_exists (chd)) // May throw (e.g., EACCES).
          return;
      }
      catch (const system_error& e)
      {
        fail << "invalid installation directory " << chd << ": " << e;
      }

      // While install -d will create all the intermediate components between
      // base and dir, we do it explicitly, one at a time. This way the output
      // is symmetrical to uninstall() below.
      //
      // Note that if the chroot directory does not exist, then install -d
      // will create it and we don't bother removing it.
      //
      if (d != base.dir)
      {
        dir_path pd (d.directory ());

        if (pd != base.dir)
          install_d (rs, base, pd, t, verbosity);
      }

      cstrings args;

      string reld (
        ctx.build_host->class_ == "windows"
        ? msys_path (chd)
        : relative (chd).string ());

      if (base.sudo != nullptr)
        args.push_back (base.sudo->c_str ());

      args.push_back (base.cmd->string ().c_str ());
      args.push_back ("-d");

      if (base.options != nullptr)
        append_options (args, *base.options);

      args.push_back ("-m");
      args.push_back (base.dir_mode->c_str ());
      args.push_back (reld.c_str ());
      args.push_back (nullptr);

      process_path pp (run_search (args[0]));

      if (verb >= verbosity)
      {
        if (verb >= 2)
          print_process (args);
        else if (verb)
          print_diag ("install -d", chd); // See also `install -l` below.
      }

      run (ctx,
           pp, args,
           verb >= verbosity ? 1 : verb_never /* finish_verbosity */);

      context_data::manifest_install_d (ctx, t, d, *base.dir_mode);
    }

    void file_rule::
    install_f (const scope& rs,
               const install_dir& base,
               const path& name,
               const file& t,
               const path& f,
               uint16_t verbosity)
    {
      assert (name.empty () || name.simple ());

      context& ctx (rs.ctx);

      const path& leaf (name.empty () ? f.leaf () : name);

      if (!filter_entry (rs, base.dir, leaf, entry_type::regular))
        return;

      path relf (relative (f));

      dir_path chd (chroot_path (rs, base.dir));

      string reld (
        ctx.build_host->class_ == "windows"
        ? msys_path (chd)
        : relative (chd).string ());

      if (!name.empty ())
      {
        reld += path::traits_type::directory_separator;
        reld += name.string ();
      }

      cstrings args;

      if (base.sudo != nullptr)
        args.push_back (base.sudo->c_str ());

      args.push_back (base.cmd->string ().c_str ());

      if (base.options != nullptr)
        append_options (args, *base.options);

      args.push_back ("-m");
      args.push_back (base.mode->c_str ());
      args.push_back (relf.string ().c_str ());
      args.push_back (reld.c_str ());
      args.push_back (nullptr);

      process_path pp (run_search (args[0]));

      if (verb >= verbosity)
      {
        if (verb >= 2)
          print_process (args);
        else if (verb)
        {
          if (name.empty ())
            print_diag ("install", t, chd);
          else
            print_diag ("install", t, chd / name);
        }
      }

      if (!ctx.dry_run)
        run (ctx,
             pp, args,
             verb >= verbosity ? 1 : verb_never /* finish_verbosity */);

      context_data::manifest_install_f (ctx, t, base.dir, leaf, *base.mode);
    }

    void file_rule::
    install_l (const scope& rs,
               const install_dir& base,
               const path& link,
               const file& target,
               const path& link_target,
               uint16_t verbosity)
    {
      assert (link.simple () && !link.empty ());

      context& ctx (rs.ctx);

      if (!filter_entry (rs, base.dir, link, entry_type::symlink))
        return;

      if (link_target.absolute () &&
          cast_false<bool> (rs["install.relocatable"]))
      {
        fail << "absolute symlink target " << link_target.string ()
             << " in relocatable installation";
      }

      dir_path chd (chroot_path (rs, base.dir));

      path rell (relative (chd));
      rell /= link;

      // We can create a symlink directly without calling ln. This, however,
      // won't work if we have sudo. Also, we would have to deal with existing
      // destinations (ln's -f takes care of that). So we are just going to
      // always (sudo or not) use ln unless we are on Windows, where we will
      // use mkanylink().
      //
#ifndef _WIN32
      const char* args_a[] = {
        base.sudo != nullptr ? base.sudo->c_str () : nullptr,
        "ln",
        "-sf",
        link_target.string ().c_str (),
        rell.string ().c_str (),
        nullptr};

      const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

      process_path pp (run_search (args[0]));

      if (verb >= verbosity)
      {
        if (verb >= 2)
          print_process (args);
        else if (verb)
        {
          // Without a flag it's unclear (unlike with ln) that we are creating
          // a link. FreeBSD install(1) has the -l flag with the appropriate
          // semantics. For consistency, we also pass -d above.
          //
          print_diag ("install -l", link_target, chd / link);
        }
      }

      if (!ctx.dry_run)
        run (ctx,
             pp, args,
             verb >= verbosity ? 1 : verb_never /* finish_verbosity */);
#else
      // The -f part.
      //
      // We use uninstall_f() since reliably removing stuff on Windows is no
      // easy feat (see uninstall_f() for details).
      //
      uninstall_f (rs, base, nullptr /* target */, link, 3 /* verbosity */);

      if (verb >= verbosity)
      {
        if (verb >= 2)
          text << "ln -sf " << link_target.string () << ' ' << rell.string ();
        else if (verb)
          print_diag ("install -l", link_target, chd / link);
      }

      if (!ctx.dry_run)
      try
      {
        mkanylink (link_target, rell, true /* copy */);
      }
      catch (const pair<entry_type, system_error>& e)
      {
        const char* w (e.first == entry_type::regular ? "copy"     :
                       e.first == entry_type::symlink ? "symlink"  :
                       e.first == entry_type::other   ? "hardlink" :
                       nullptr);

        fail << "unable to make " << w << ' ' << rell << ": " << e.second;
      }
#endif

      context_data::manifest_install_l (ctx,
                                        target,
                                        link_target,
                                        base.dir,
                                        link);
    }

    target_state file_rule::
    perform_install (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());
      const path& tp (t.path ());

      // Path should have been assigned by update unless it is unreal.
      //
      assert (!tp.empty () || t.mtime () == timestamp_unreal);

      const scope& rs (t.root_scope ());

      auto install_target = [&rs, this] (const file& t,
                                         const path& p,
                                         uint16_t verbosity)
      {
        // Note: similar logic to resolve_file().
        //
        bool n (!p.to_directory ());
        dir_path d (n ? p.directory () : path_cast<dir_path> (p));

        if (n && d.empty ())
          fail << "relative installation file path '" << p
               << "' has no directory component";

        // Resolve target directory.
        //
        install_dirs ids (resolve (t, d));

        // Handle install.subdirs if one was specified. Unless the target path
        // includes the file name in which case we assume it's a "final" path.
        //
        if (!n)
        {
          if (auto l = t["install.subdirs"])
          {
            if (cast<bool> (l))
              resolve_subdir (ids, t, t.base_scope (), l);
          }
        }

        // Create leading directories. Note that we are using the leading
        // directory (if there is one) for the creation information (mode,
        // sudo, etc).
        //
        for (auto i (ids.begin ()), j (i); i != ids.end (); j = i++)
          install_d (rs, *j, i->dir, t, verbosity); // install -d

        install_dir& id (ids.back ());

        // Override mode if one was specified.
        //
        if (auto l = t["install.mode"])
          id.mode = &cast<string> (l);

        // Install the target.
        //
        auto_rmfile f (install_pre (t, id));

        // If install_pre() returned a different file name, make sure we
        // install it as the original.
        //
        const path& tp (t.path ());
        const path& fp (f.path);

        install_f (
          rs,
          id,
          n ? p.leaf () : fp.leaf () != tp.leaf () ? tp.leaf () : path (),
          t,
          f.path,
          verbosity);

        install_post (t, id, move (f));
      };

      // First handle installable prerequisites.
      //
      target_state r (straight_execute_prerequisites (a, t));

      bool fr (filter (a, t, t));

      // Then installable ad hoc group members, if any.
      //
      for (const target* m (t.adhoc_member);
           m != nullptr;
           m = m->adhoc_member)
      {
        if (const file* mf = m->is_a<file> ())
        {
          if (!mf->path ().empty () && mf->mtime () != timestamp_nonexistent)
          {
            if (filter (a, t, *mf))
            {
              if (const path* p = lookup_install<path> (*mf, "install"))
              {
                install_target (*mf, *p, !fr || tp.empty () ? 1 : 2);
                r |= target_state::changed;
              }
            }
          }
        }
      }

      // Finally install the target itself (since we got here we know the
      // install variable is there).
      //
      if (fr && !tp.empty ())
      {
        install_target (t, cast<path> (t[var_install (rs)]), 1);
        r |= target_state::changed;
      }

      return r;
    }

    bool file_rule::
    uninstall_d (const scope& rs,
                 const install_dir& base,
                 const dir_path& d,
                 uint16_t verbosity)
    {
      assert (d.absolute ());

      context& ctx (rs.ctx);

      // See install_d() for the rationale.
      //
      if (ctx.dry_run || !filter_entry (rs, d, path (), entry_type::directory))
        return false;

      dir_path chd (chroot_path (rs, d));

      // Figure out if we should try to remove this directory. Note that if
      // it doesn't exist, then we may still need to remove outer ones.
      //
      bool r (false);
      try
      {
        if ((r = dir_exists (chd))) // May throw (e.g., EACCES).
        {
          if (!dir_empty (chd)) // May also throw.
            return false; // Won't be able to remove any outer directories.
        }
      }
      catch (const system_error& e)
      {
        fail << "invalid installation directory " << chd << ": " << e;
      }

      if (r)
      {
        dir_path reld (relative (chd));

        // Normally when we need to remove a file or directory we do it
        // directly without calling rm/rmdir. This however, won't work if we
        // have sudo. So we are going to do it both ways.
        //
        // While there is no sudo on Windows, deleting things that are being
        // used can get complicated. So we will always use rm/rmdir from
        // MSYS2/Cygwin which go above and beyond to accomplish the mission.
        //
        // Note also that it's possible we didn't create the directory and
        // won't be able to remove it due to permissions (for example, on Mac
        // OS we cannot remove empty /usr/local even with sudo). So instead of
        // failing we issue a warning and skip the directory.
        //
#ifndef _WIN32
        if (base.sudo == nullptr)
        {
          if (verb >= verbosity)
          {
            if (verb >= 2)
              text << "rmdir " << reld;
            else if (verb)
              print_diag ("uninstall -d", chd);
          }

          try
          {
            try_rmdir (chd);
          }
          catch (const system_error&)
          {
            r = false;
          }
        }
        else
#endif
        {
          const char* args_a[] = {
            base.sudo != nullptr ? base.sudo->c_str () : nullptr,
            "rmdir",
            reld.string ().c_str (),
            nullptr};

          const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

          process_path pp (run_search (args[0]));

          if (verb >= verbosity)
          {
            if (verb >= 2)
              print_process (args);
            else if (verb)
              print_diag ("uninstall -d", chd);
          }

          process pr (run_start (pp, args,
                                 0                       /* stdin */,
                                 1                       /* stdout */,
                                 diag_buffer::pipe (ctx) /* stderr */));
          diag_buffer dbuf (ctx, args[0], pr);
          dbuf.read ();
          r = run_finish_code (
            dbuf,
            args, pr,
            verb >= verbosity ? 1 : verb_never /* verbosity */);
        }

        if (!r)
        {
          warn << "unable to remove empty directory " << chd << ", ignoring";
          return false;
        }
      }

      // If we have more empty directories between base and dir, then try
      // to clean them up as well.
      //
      if (d != base.dir)
      {
        dir_path pd (d.directory ());

        if (pd != base.dir)
          r = uninstall_d (rs, base, pd, verbosity) || r;
      }

      return r;
    }

    static void
    uninstall_f_impl (const scope& rs,
                      const install_dir& base,
                      const path& f,
                      uint16_t verbosity)
    {
      context& ctx (rs.ctx);

      path relf (relative (f));

      // The same story as with uninstall -d (on Windows rm is also from
      // MSYS2/Cygwin).
      //
#ifndef _WIN32
      if (base.sudo == nullptr)
      {
        if (verb >= verbosity && verb >= 2)
          text << "rm " << relf;

        if (!ctx.dry_run)
        {
          try
          {
            try_rmfile (f);
          }
          catch (const system_error& e)
          {
            fail << "unable to remove file " << f << ": " << e;
          }
        }
      }
      else
#endif
      {
        const char* args_a[] = {
          base.sudo != nullptr ? base.sudo->c_str () : nullptr,
          "rm",
          "-f",
          relf.string ().c_str (),
          nullptr};

        const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

        process_path pp (run_search (args[0]));

        if (verb >= verbosity)
        {
          if (verb >= 2)
            print_process (args);
        }

        if (!ctx.dry_run)
          run (ctx,
               pp, args,
               verb >= verbosity ? 1 : verb_never /* finish_verbosity */);
      }
    }

    bool file_rule::
    uninstall_f (const scope& rs,
                 const install_dir& base,
                 const file* t,
                 const path& name,
                 uint16_t verbosity)
    {
      assert (name.empty () ? t != nullptr : name.simple ());

      const path& leaf (name.empty () ? t->path ().leaf () : name);

      if (!filter_entry (rs, base.dir, leaf, entry_type::regular))
        return false;

      dir_path chd (chroot_path (rs, base.dir));
      path f (chd / leaf);

      try
      {
        // Note: don't follow symlinks so if the target is a dangling symlinks
        // we will proceed to removing it.
        //
        if (!file_exists (f, false)) // May throw (e.g., EACCES).
          return false;
      }
      catch (const system_error& e)
      {
        fail << "invalid installation path " << f << ": " << e;
      }

      if (verb >= verbosity && verb == 1)
      {
        if (t != nullptr)
        {
          if (name.empty ())
            print_diag ("uninstall", *t, chd, "<-");
          else
            print_diag ("uninstall", *t, f, "<-");
        }
        else
          print_diag ("uninstall", f);
      }

      uninstall_f_impl (rs, base, f, verbosity);
      return true;
    }

    bool file_rule::
    uninstall_l (const scope& rs,
                 const install_dir& base,
                 const path& link,
                 const path& /*link_target*/,
                 uint16_t verbosity)
    {
      assert (link.simple () && !link.empty ());

      if (!filter_entry (rs, base.dir, link, entry_type::symlink))
        return false;

      dir_path chd (chroot_path (rs, base.dir));
      path f (chd / link);

      try
      {
        // Note: don't follow symlinks so if the target is a dangling symlinks
        // we will proceed to removing it.
        //
        if (!file_exists (f, false)) // May throw (e.g., EACCES).
          return false;
      }
      catch (const system_error& e)
      {
        fail << "invalid installation path " << f << ": " << e;
      }

      if (verb >= verbosity && verb == 1)
      {
        // It's dubious showing the link target path adds anything useful
        // here.
        //
#if 0
        print_diag ("uninstall -l", target, f, "<-");
#else
        print_diag ("uninstall -l", f);
#endif
      }

      uninstall_f_impl (rs, base, f, verbosity);
      return true;
    }

    target_state file_rule::
    perform_uninstall (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());
      const path& tp (t.path ());

      // Path should have been assigned by update unless it is unreal.
      //
      assert (!tp.empty () || t.mtime () == timestamp_unreal);

      const scope& rs (t.root_scope ());

      auto uninstall_target = [&rs, this] (const file& t,
                                           const path& p,
                                           uint16_t verbosity) -> target_state
      {
        bool n (!p.to_directory ());
        dir_path d (n ? p.directory () : path_cast<dir_path> (p));

        if (n && d.empty ())
          fail << "relative installation file path '" << p
               << "' has no directory component";

        // Resolve target directory.
        //
        install_dirs ids (resolve (t, d));

        // Handle install.subdirs if one was specified.
        //
        if (!n)
        {
          if (auto l = t["install.subdirs"])
          {
            if (cast<bool> (l))
              resolve_subdir (ids, t, t.base_scope (), l);
          }
        }

        // Remove extras and the target itself.
        //
        const install_dir& id (ids.back ());

        target_state r (uninstall_extra (t, id)
                        ? target_state::changed
                        : target_state::unchanged);

        if (uninstall_f (rs, id, &t, n ? p.leaf () : path (), verbosity))
          r |= target_state::changed;

        // Clean up empty leading directories (in reverse).
        //
        // Note that we are using the leading directory (if there is one) for
        // the clean up information (sudo, etc). We may also try to uninstall
        // the same directory via different bases (e.g., root and exec_bin).
        //
        for (auto i (ids.rbegin ()), j (i), e (ids.rend ()); i != e; j = ++i)
        {
          if (uninstall_d (rs, ++j != e ? *j : *i, i->dir, verbosity))
            r |= target_state::changed;
        }

        return r;
      };

      // Reverse order of installation: first the target itself (since we got
      // here we know the install variable is there).
      //
      target_state r (target_state::unchanged);

      bool fr (filter (a, t, t));

      if (fr && !tp.empty ())
        r |= uninstall_target (t, cast<path> (t[var_install (rs)]), 1);

      // Then installable ad hoc group members, if any. To be anally precise,
      // we would have to do it in reverse, but that's not easy (it's a
      // single-linked list).
      //
      for (const target* m (t.adhoc_member);
           m != nullptr;
           m = m->adhoc_member)
      {
        if (const file* mf = m->is_a<file> ())
        {
          if (!mf->path ().empty () && mf->mtime () != timestamp_nonexistent)
          {
            if (filter (a, t, *mf))
            {
              if (const path* p = lookup_install<path> (*m, "install"))
              {
                r |= uninstall_target (
                  *mf,
                  *p,
                  !fr || tp.empty () || r != target_state::changed ? 1 : 2);
              }
            }
          }
        }
      }

      // Finally handle installable prerequisites.
      //
      r |= reverse_execute_prerequisites (a, t);

      return r;
    }

    // fsdir_rule
    //
    const fsdir_rule fsdir_rule::instance;

    bool fsdir_rule::
    match (action, target&) const
    {
      // We always match.
      //
      // Note that we are called both as the outer part during the update-for-
      // un/install pre-operation and as the inner part during the un/install
      // operation itself.
      //
      return true;
    }

    recipe fsdir_rule::
    apply (action a, target& t) const
    {
      // If this is outer part of the update-for-un/install, delegate to the
      // default fsdir rule. Otherwise, this is a noop (we don't install
      // fsdir{}).
      //
      // For now we also assume we don't need to do anything for prerequisites
      // (the only sensible prerequisite of fsdir{} is another fsdir{}).
      //
      if (a.operation () == update_id)
      {
        match_inner (a, t);
        return inner_recipe;
      }
      else
        return noop_recipe;
    }
  }
}
