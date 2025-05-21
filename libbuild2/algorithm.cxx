// file      : libbuild2/algorithm.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/algorithm.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/rule.hxx>
#include <libbuild2/file.hxx> // import()
#include <libbuild2/search.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/operation.hxx> // perform_{match,execute}()
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite.hxx>
#include <libbuild2/functions-name.hxx> // to_target()

using namespace std;
using namespace butl;

namespace build2
{
  const target&
  search (const target& t, const prerequisite& p)
  {
    assert (t.ctx.phase == run_phase::match);

    const target* r (p.target.load (memory_order_consume));

    if (r == nullptr)
      r = &search_custom (p, search (t, p.key ()));

    return *r;
  }

  const target*
  search_existing (const prerequisite& p)
  {
    context& ctx (p.scope.ctx);

    assert (ctx.phase == run_phase::match || ctx.phase == run_phase::execute);

    const target* r (p.target.load (memory_order_consume));

    if (r == nullptr)
    {
      r = search_existing (ctx, p.key ());

      if (r != nullptr)
        search_custom (p, *r);
    }

    return r;
  }

  const target&
  search (const target& t, const prerequisite_key& pk)
  {
    context& ctx (t.ctx);

    assert (ctx.phase == run_phase::match);

    // If this is a project-qualified prerequisite, then this is import's
    // business (phase 2).
    //
    if (pk.proj)
      return import2 (ctx, pk);

    if (const target* pt = pk.tk.type->search (ctx, &t, pk))
      return *pt;

    if (pk.tk.out->empty ())
      return create_new_target (ctx, pk);

    // If this is triggered, then you are probably not passing scope to
    // search() (which leads to search_existing_file() being skipped).
    //
    fail << "no existing source file for prerequisite " << pk << endf;
  }

  pair<target&, ulock>
  search_locked (const target& t, const prerequisite_key& pk)
  {
    context& ctx (t.ctx);

    assert (ctx.phase == run_phase::match && !pk.proj);

    if (const target* pt = pk.tk.type->search (ctx, &t, pk))
      return {const_cast<target&> (*pt), ulock ()};

    if (pk.tk.out->empty ())
      return create_new_target_locked (ctx, pk);

    // If this is triggered, then you are probably not passing scope to
    // search() (which leads to search_existing_file() being skipped).
    //
    fail << "no existing source file for prerequisite " << pk << endf;
  }

  const target*
  search_existing (context& ctx, const prerequisite_key& pk)
  {
    return pk.proj
      ? import_existing (ctx, pk)
      : pk.tk.type->search (ctx, nullptr /* existing */, pk);
  }

  const target&
  search_new (context& ctx, const prerequisite_key& pk)
  {
    assert (ctx.phase == run_phase::load || ctx.phase == run_phase::match);

    if (const target* pt = search_existing_target (ctx, pk, true /*out_only*/))
      return *pt;

    return create_new_target (ctx, pk);
  }

  pair<target&, ulock>
  search_new_locked (context& ctx, const prerequisite_key& pk)
  {
    assert (ctx.phase == run_phase::load || ctx.phase == run_phase::match);

    if (const target* pt = search_existing_target (ctx, pk, true /*out_only*/))
      return {const_cast<target&> (*pt), ulock ()};

    return create_new_target_locked (ctx, pk);
  }

  const target&
  search (const target& t, name&& n, const scope& s, const target_type* tt)
  {
    assert (t.ctx.phase == run_phase::match);

    auto rp (s.find_target_type (n, location (), tt));
    tt = rp.first;
    optional<string>& ext (rp.second);

    if (tt == nullptr)
      fail << "unknown target type " << n.type << " in name " << n;

    if (!n.dir.empty ())
      n.dir.normalize (false, true); // Current dir collapses to an empty one.

    // @@ OUT: for now we assume the prerequisite's out is undetermined.
    //         Would need to pass a pair of names.
    //
    return search (t,
                   *tt,
                   n.dir,
                   dir_path (),
                   n.value,
                   ext ? &*ext : nullptr,
                   &s,
                   n.proj);
  }

  const target*
  search_existing (const name& cn, const scope& s, const dir_path& out)
  {
    // See also scope::find_prerequisite_key().
    //
    name n (cn);
    auto rp (s.find_target_type (n, location ()));
    const target_type* tt (rp.first);
    optional<string>& ext (rp.second);

    // For now we treat an unknown target type as an unknown target. Seems
    // logical.
    //
    if (tt == nullptr)
      return nullptr;

    if (!n.dir.empty ())
    {
      // More often than not a non-empty directory will already be normalized.
      //
      // Note that we collapse current dir to an empty one.
      //
      if (!n.dir.normalized () || n.dir.string () == ".")
        n.dir.normalize (false, true);
    }

    bool q (cn.qualified ());
    prerequisite_key pk {
      n.proj, {tt, &n.dir, q ? &empty_dir_path : &out, &n.value, ext}, &s};

    return q
      ? import_existing (s.ctx, pk)
      : tt->search (s.ctx, nullptr /* existing */, pk);
  }

  const target*
  search_existing (const names& ns, const scope& s)
  {
    if (size_t n = ns.size ())
    {
      if (n == (ns[0].pair ? 2 : 1))
      {
        return search_existing (ns[0], s, n == 1 ? dir_path () : ns[1].dir);
      }
    }

    fail << "invalid target name: " << ns << endf;
  }

  // target_lock
  //
  // Note that the stack may contain locks for targets from multiple nested
  // contexts. This should be harmless (deadlock detection-wise) since
  // contexts are assumed non-overlapping.
  //
  static
#ifdef __cpp_thread_local
  thread_local
#else
  __thread
#endif
  const target_lock* target_lock_stack = nullptr;

  const target_lock* target_lock::
  stack () noexcept
  {
    return target_lock_stack;
  }

  const target_lock* target_lock::
  stack (const target_lock* s) noexcept
  {
    const target_lock* r (target_lock_stack);
    target_lock_stack = s;
    return r;
  }

  // If the work_queue is absent, then we don't wait.
  //
  // While already applied or executed targets are normally not locked, if
  // options contain any bits that are not already in cur_options, then the
  // target is locked even in these states.
  //
  target_lock
  lock_impl (action a, const target& ct,
             optional<scheduler::work_queue> wq,
             uint64_t options)
  {
    context& ctx (ct.ctx);

    assert (ctx.phase == run_phase::match);

    // Most likely the target's state is (count_touched - 1), that is, 0 or
    // previously executed, so let's start with that.
    //
    size_t b (ctx.count_base ());
    size_t e (b + target::offset_touched - 1);

    size_t appl (b + target::offset_applied);
    size_t busy (b + target::offset_busy);

    const target::opstate& cs (ct[a]);
    atomic_count& task_count (cs.task_count);

    while (!task_count.compare_exchange_strong (
             e,
             busy,
             memory_order_acq_rel,  // Synchronize on success.
             memory_order_acquire)) // Synchronize on failure.
    {
      // Wait for the count to drop below busy if someone is already working
      // on this target.
      //
      if (e >= busy)
      {
        // Check for dependency cycles. The cycle members should be evident
        // from the "while ..." info lines that will follow.
        //
        if (dependency_cycle (a, ct))
          fail << "dependency cycle detected involving target " << ct;

        if (!wq)
          return target_lock {a, nullptr, e - b, false};

        // @@ TMP: remove if the queue_mark approach pans out.
        //
#if 0
        // Don't work the queue if in the update during load during
        // interrupting load. Failed that we may deadlock via dir_search().
        //
        if (ctx.update_during_load > 1)
          *wq = scheduler::work_none;
#endif

        // We also unlock the phase for the duration of the wait. Why?
        // Consider this scenario: we are trying to match a dir{} target whose
        // buildfile still needs to be loaded. Let's say someone else started
        // the match before us. So we wait for their completion and they wait
        // to switch the phase to load. Which would result in a deadlock
        // unless we release the phase.
        //
        phase_unlock u (ct.ctx, true /* delay */);
        e = ctx.sched->wait (busy - 1, task_count, u, *wq);
      }

      // We don't lock already applied or executed targets unless there
      // are new options.
      //
      // Note: we don't have the lock yet so we must use atomic cur_options.
      // We also have to re-check this once we've grabbed the lock.
      //
      if (e >= appl &&
          (cs.match_extra.cur_options_.load (memory_order_relaxed) & options)
          == options)
        return target_lock {a, nullptr, e - b, false};
    }

    // We now have the lock. Analyze the old value and decide what to do.
    //
    target& t (const_cast<target&> (ct));
    target::opstate& s (t[a]);

    size_t offset;
    bool first;
    if ((first = (e <= b)))
    {
      // First lock for this operation.
      //
      // Note that we use 0 match_extra::cur_options_ as an indication of not
      // being applied yet. In particular, in the match phase, this is used to
      // distinguish between the "busy because not applied yet" and "busy
      // because relocked to reapply match options" cases. See
      // target::matched() for details.
      //
      s.rule = nullptr;
      s.dependents.store (0, memory_order_release);
      s.match_extra.cur_options_.store (0, memory_order_relaxed);

      offset = target::offset_touched;
    }
    else
    {
      // Re-check the options if already applied or worse.
      //
      if (e >= appl && (s.match_extra.cur_options & options) == options)
      {
        // Essentially unlock_impl().
        //
        task_count.store (e, memory_order_release);
        ctx.sched->resume (task_count);

        return target_lock {a, nullptr, e - b, false};
      }

      offset = e - b;
    }

    return target_lock {a, &t, offset, first};
  }

  void
  unlock_impl (action a, target& t, size_t offset)
  {
    context& ctx (t.ctx);

    assert (ctx.phase == run_phase::match);

    atomic_count& task_count (t[a].task_count);

    // Set the task count and wake up any threads that might be waiting for
    // this target.
    //
    task_count.store (offset + ctx.count_base (), memory_order_release);
    ctx.sched->resume (task_count);
  }

  target&
  add_adhoc_member (target& t,
                    const target_type& tt,
                    dir_path dir,
                    dir_path out,
                    string n,
                    optional<string> ext)
  {
    tracer trace ("add_adhoc_member");

    const_ptr<target>* mp (&t.adhoc_member);
    for (; *mp != nullptr && !(*mp)->is_a (tt); mp = &(*mp)->adhoc_member) ;

    if (*mp != nullptr) // Might already be there.
      return **mp;

    pair<target&, ulock> r (
      t.ctx.targets.insert_locked (tt,
                                   move (dir),
                                   move (out),
                                   move (n),
                                   move (ext),
                                   target_decl::implied,
                                   trace,
                                   true /* skip_find */));

    target& m (r.first);

    if (!r.second)
      fail << "target " << m << " already exists and cannot be made "
           << "ad hoc member of group " << t;

    m.group = &t;
    *mp = &m;

    return m;
  };

  pair<target&, bool>
  add_adhoc_member_identity (target& t,
                             const target_type& tt,
                             dir_path dir,
                             dir_path out,
                             string n,
                             optional<string> ext,
                             const location& loc)
  {
    // NOTE: see similar code in parser::enter_adhoc_members().

    tracer trace ("add_adhoc_member_identity");

    pair<target&, ulock> r (
      t.ctx.targets.insert_locked (tt,
                                   move (dir),
                                   move (out),
                                   move (n),
                                   move (ext),
                                   target_decl::implied,
                                   trace,
                                   true /* skip_find */));
    target& m (r.first);

    // Add as an ad hoc member at the end of the chain skipping duplicates.
    //
    const_ptr<target>* mp (&t.adhoc_member);
    for (; *mp != nullptr; mp = &(*mp)->adhoc_member)
    {
      if (*mp == &m)
        return {m, false};
    }

    if (!r.second)
      fail (loc) << "target " << m << " already exists and cannot be made "
                 << "ad hoc member of group " << t;

    m.group = &t;
    *mp = &m;

    return {m, true};
  }

  static bool
  trace_target (const target& t, const vector<name>& ns)
  {
    for (const name& n: ns)
    {
      if (n.untyped () || n.qualified () || n.pattern)
        fail << "unsupported trace target name '" << n << "'" <<
          info << "unqualified, typed, non-pattern name expected";

      if (!n.dir.empty ())
      {
        if (n.dir.relative () || !n.dir.normalized ())
          fail << "absolute and normalized trace target directory expected";

        if (t.dir != n.dir)
          continue;
      }

      if (n.type == t.type ().name && n.value == t.name)
        return true;
    }

    return false;
  }

  void
  set_rule_trace (target_lock& l, const rule_match* rm)
  {
    action a (l.action);
    target& t (*l.target);

    // Note: see similar code in execute_impl() for execute.
    //
    if (trace_target (t, *t.ctx.trace_match))
    {
      diag_record dr (info);

      dr << "matching to " << diag_do (a, t);

      if (rm != nullptr)
      {
        const rule& r (rm->second);

        if (const adhoc_rule* ar = dynamic_cast<const adhoc_rule*> (&r))
        {
          dr << info (ar->loc);

          if (ar->pattern != nullptr)
            dr << "using ad hoc pattern rule ";
          else
            dr << "using ad hoc recipe ";
        }
        else
          dr << info << "using rule ";

        dr << rm->first;
      }
      else
        dr << info << "using directly-assigned recipe";
    }

    t[a].rule = rm;
  }

  // Note: not static since also called by rule::sub_match().
  //
  const rule_match*
  match_adhoc_recipe (action a, target& t, match_extra& me)
  {
    auto df = make_diag_frame (
      [a, &t](const diag_record& dr)
      {
        if (verb != 0)
          dr << info << "while matching ad hoc recipe to " << diag_do (a, t);
      });

    auto match = [a, &t, &me] (const adhoc_rule& r, bool fallback) -> bool
    {
      me.reinit (fallback);

      if (auto* f = (a.outer ()
                     ? t.ctx.current_outer_oif
                     : t.ctx.current_inner_oif)->adhoc_match)
        return f (r, a, t, string () /* hint */, me);
      else
        return r.match (a, t, string () /* hint */, me);
    };

    // The action could be Y-for-X while the ad hoc recipes are always for
    // X. So strip the Y-for part for comparison (but not for the match()
    // calls; see below for the hairy inner/outer semantics details).
    //
    action ca (a.inner ()
               ? a
               : action (a.meta_operation (), a.outer_operation ()));

    // If returned rule_match is NULL, then the second half indicates whether
    // the rule was found (but did not match).
    //
    auto find_match = [&t, &match] (action ca) -> pair<const rule_match*, bool>
    {
      // Note that there can be at most one recipe for any action.
      //
      auto b (t.adhoc_recipes.begin ()), e (t.adhoc_recipes.end ());
      auto i (find_if (
                b, e,
                [ca] (const shared_ptr<adhoc_rule>& r)
                {
                  auto& as (r->actions);
                  return find (as.begin (), as.end (), ca) != as.end ();
                }));

      bool f (i != e);
      if (f)
      {
        if (!match (**i, false /* fallback */))
          i = e;
      }
      else
      {
        // See if we have a fallback implementation.
        //
        // See the adhoc_rule::reverse_fallback() documentation for details on
        // what's going on here.
        //
        // Note that it feels natural not to look for a fallback if a custom
        // recipe was provided but did not match.
        //
        const target_type& tt (t.type ());
        i = find_if (
          b, e,
          [ca, &tt] (const shared_ptr<adhoc_rule>& r)
          {
            // Only the rule that provides the "forward" action can provide
            // "reverse", so there can be at most one such rule.
            //
            return r->reverse_fallback (ca, tt);
          });

        f = (i != e);
        if (f)
        {
          if (!match (**i, true /* fallback */))
            i = e;
        }
      }

      return pair<const rule_match*, bool> (
        i != e ? &(*i)->rule_match : nullptr,
        f);
    };

    pair<const rule_match*, bool> r (find_match (ca));

    // Provide the "add dist_* and configure_* actions for every perform_*
    // action unless there is a custom one" semantics (see the equivalent ad
    // hoc rule registration code in the parser for background).
    //
    // Note that handling this in the parser by adding the extra actions is
    // difficult because we store recipe actions in the recipe itself (
    // adhoc_rule::actions) and a recipe could be shared among multiple
    // targets, some of which may provide a "custom one" as another recipe. On
    // the other hand, handling it here is relatively straightforward.
    //
    if (r.first == nullptr && !r.second)
    {
      meta_operation_id mo (ca.meta_operation ());
      if (mo == configure_id || mo == dist_id)
      {
        action pa (perform_id, ca.operation ());
        r = find_match (pa);
      }
    }

    return r.first;
  }

  // Return the matching rule or NULL if no match and try_match is true.
  //
  const rule_match*
  match_rule_impl (action a, target& t,
                   uint64_t options,
                   const rule* skip,
                   bool try_match,
                   match_extra* pme)
  {
    using fallback_rule = adhoc_rule_pattern::fallback_rule;

    auto adhoc_rule_match = [] (const rule_match& r)
    {
      return dynamic_cast<const adhoc_rule*> (&r.second.get ());
    };

    auto fallback_rule_match = [] (const rule_match& r)
    {
      return dynamic_cast<const fallback_rule*> (&r.second.get ());
    };

    // Note: we copy the options value to me.new_options after successfully
    // matching the rule to make sure rule::match() implementations don't rely
    // on it.
    //
    match_extra& me (pme == nullptr ? t[a].match_extra : *pme);

    if (const target* g = t.group)
    {
      // If this is a group with dynamic members, then match it with the
      // group's rule automatically. See dyndep_rule::inject_group_member()
      // for background.
      //
      if ((g->type ().flags & target_type::flag::dyn_members) ==
          target_type::flag::dyn_members)
      {
        if (g->matched (a, memory_order_acquire))
        {
          const rule_match* r (g->state[a].rule);
          assert (r != nullptr); // Shouldn't happen with dyn_members.

          me.new_options = options;
          return r;
        }

        // Assume static member and fall through.
      }

      // If this is a member of group-based target, then first try to find a
      // matching ad hoc recipe/rule by matching (to an ad hoc recipe/rule)
      // the group but applying to the member. See adhoc_rule::match() for
      // background, including for why const_cast should be safe.
      //
      // To put it another way, if a group is matched by an ad hoc
      // recipe/rule, then we want all the member to be matched to the same
      // recipe/rule.
      //
      // Note that such a group is dyn_members so we would have tried the
      // "already matched" case above.
      //
      if (g->is_a<group> ())
      {
        // We cannot init match_extra from the target if it's unlocked so use
        // a temporary (it shouldn't be modified if unlocked).
        //
        match_extra gme (false /* locked */);
        if (const rule_match* r = match_rule_impl (a, const_cast<target&> (*g),
                                                   0 /* options */,
                                                   skip,
                                                   true /* try_match */,
                                                   &gme))
        {
          me.new_options = options;
          return r;
        }

        // Fall through to normal match of the member.
      }
    }

    const scope& bs (t.base_scope ());

    // Match rules in project environment.
    //
    auto_project_env penv;
    if (const scope* rs = bs.root_scope ())
      penv = auto_project_env (*rs);

    // First check for an ad hoc recipe.
    //
    // Note that a fallback recipe is preferred over a non-fallback rule.
    //
    if (!t.adhoc_recipes.empty ())
    {
      if (const rule_match* r = match_adhoc_recipe (a, t, me))
      {
        me.new_options = options;
        return r;
      }
    }

    // If this is an outer operation (Y-for-X), then we look for rules
    // registered for the outer id (X; yes, it's really outer). Note that we
    // still pass the original action to the rule's match() function so that
    // it can distinguish between a pre/post operation (Y-for-X) and the
    // actual operation (X).
    //
    // If you are then wondering how would a rule for Y ever match in case of
    // Y-for-X, the answer is via a rule that matches for X and then, in case
    // of Y-for-X, matches an inner rule for just Y (see match_inner()).
    //
    meta_operation_id mo (a.meta_operation ());
    operation_id o (a.inner () ? a.operation () : a.outer_operation ());

    // Our hint semantics applies regardless of the meta-operation. This works
    // reasonably well except for the default/fallback rules provided by some
    // meta-operations (e.g., dist, config), which naturally do not match the
    // hint.
    //
    // The way we solve this problem is by trying a hint-less match as a
    // fallback for non-perform meta-operations. @@ Ideally we would want to
    // only consider such default/fallback rules, which we may do in the
    // future (we could just decorate their names with some special marker,
    // e.g., `dist.file.*` but that would be visible in diagnostics).
    //
    // It seems the only potential problem with this approach is the inability
    // by the user to specify the hint for this specific meta-operation (e.g.,
    // to resolve an ambiguity between two rules or override a matched rule),
    // which seems quite remote at the moment. Maybe/later we can invent a
    // syntax for that.
    //
    const string* hint;
    for (bool retry (false);; retry = true)
    {
      hint = retry
        ? &empty_string
        : &t.find_hint (o); // MT-safe (target locked).

      for (auto tt (&t.type ()); tt != nullptr; tt = tt->base)
      {
        // Search scopes outwards, stopping at the project root. For retry
        // only look in the root and global scopes.
        //
        for (const scope* s (retry ? bs.root_scope () : &bs);
             s != nullptr;
             s = s->root () ? &s->global_scope () : s->parent_scope ())
        {
          const operation_rule_map* om (s->rules[mo]);

          if (om == nullptr)
            continue; // No entry for this meta-operation id.

          // First try the map for the actual operation. If that doesn't yeld
          // anything, try the wildcard map.
          //
          for (operation_id oi (o), oip (o); oip != 0; oip = oi, oi = 0)
          {
            const target_type_rule_map* ttm ((*om)[oi]);

            if (ttm == nullptr)
              continue; // No entry for this operation id.

            if (ttm->empty ())
              continue; // Empty map for this operation id.

            auto i (ttm->find (tt));

            if (i == ttm->end () || i->second.empty ())
              continue; // No rules registered for this target type.

            const auto& rules (i->second); // Name map.

            // Filter against the hint, if any.
            //
            auto rs (hint->empty ()
                     ? make_pair (rules.begin (), rules.end ())
                     : rules.find_sub (*hint));

            for (auto i (rs.first); i != rs.second; ++i)
            {
              const rule_match* r (&*i);

              // In a somewhat hackish way we reuse operation wildcards to
              // plumb the ad hoc rule's reverse operation fallback logic.
              //
              // The difficulty is two-fold:
              //
              // 1. It's difficult to add the fallback flag to the rule map
              //    because of rule_match which is used throughout.
              //
              // 2. Even if we could do that, we pass the reverse action to
              //    reverse_fallback() rather than it returning (a list) of
              //    reverse actions, which would be necessary to register them.
              //
              auto find_fallback = [mo, o, tt] (const fallback_rule& fr)
                -> const rule_match*
              {
                for (const shared_ptr<adhoc_rule>& ar: fr.rules)
                  if (ar->reverse_fallback (action (mo, o), *tt))
                    return &ar->rule_match;

                return nullptr;
              };

              if (oi == 0)
              {
                if (const fallback_rule* fr = fallback_rule_match (*r))
                {
                  if ((r = find_fallback (*fr)) == nullptr)
                    continue;
                }
              }

              // Skip non-ad hoc rules if the target is not locked (see above;
              // note that in this case match_extra is a temporary which we
              // can reinit).
              //
              if (!me.locked && !adhoc_rule_match (*r))
                continue;

              const string& n (r->first);
              const rule& ru (r->second);

              if (&ru == skip)
                continue;

              me.reinit (oi == 0 /* fallback */);
              {
                auto df = make_diag_frame (
                  [a, &t, &n](const diag_record& dr)
                  {
                    if (verb != 0)
                      dr << info << "while matching rule " << n << " to "
                         << diag_do (a, t);
                  });

                if (!ru.match (a, t, *hint, me))
                  continue;
              }

              // Do the ambiguity test.
              //
              bool ambig (false);

              maybe_diag_record dr;
              for (++i; i != rs.second; ++i)
              {
                const rule_match* r1 (&*i);

                if (oi == 0)
                {
                  if (const fallback_rule* fr = fallback_rule_match (*r1))
                  {
                    if ((r1 = find_fallback (*fr)) == nullptr)
                      continue;
                  }
                }

                if (!me.locked && !adhoc_rule_match (*r1))
                  continue;

                const string& n1 (r1->first);
                const rule& ru1 (r1->second);

                {
                  auto df = make_diag_frame (
                    [a, &t, &n1](const diag_record& dr)
                    {
                      if (verb != 0)
                        dr << info << "while matching rule " << n1 << " to "
                           << diag_do (a, t);
                    });

                  // @@ TODO: this makes target state in match() undetermined
                  //    so need to fortify rules that modify anything in match
                  //    to clear things.
                  //
                  // @@ Can't we temporarily swap things out in target?
                  //
                  match_extra me1 (me.locked, oi == 0 /* fallback */);
                  if (!ru1.match (a, t, *hint, me1))
                    continue;
                }

                if (!ambig)
                {
                  dr << fail << "multiple rules matching " << diag_doing (a, t)
                     << info << "rule " << n << " matches";
                  ambig = true;
                }

                dr << info << "rule " << n1 << " also matches";
              }

              if (!ambig)
              {
                me.new_options = options;
                return r;
              }
              else
                dr << info << "use rule hint to disambiguate this match";
            }
          }
        }
      }

      if (mo == perform_id || hint->empty () || retry)
        break;
    }

    me.free ();

    if (!try_match)
    {
      diag_record dr (fail);

      if (hint->empty ())
        dr << "no rule to ";
      else
        dr << "no rule with hint " << *hint << " to ";

      dr << diag_do (a, t);

      // Try to give some hints of the common causes.
      //
      switch (t.decl)
      {
      case target_decl::prereq_new:
        {
          dr << info << "target " << t << " is not declared in any buildfile";

          if (t.is_a<file> ())
            dr << info << "perhaps it is a missing source file?";

          break;
        }
      case target_decl::prereq_file:
        {
          // It's an existing file so it's an unlikely case.
          //
          break;
        }
      case target_decl::implied:
        {
          // While the "in a buildfile" is not exactly accurate, we assume
          // it's unlikely we will end up here in other cases.
          //
          dr << info << "target " << t << " is implicitly declared in a "
             << "buildfile";

          if (const scope* rs = bs.root_scope ())
          {
            if (t.out.empty () && rs->src_path () != rs->out_path ())
            {
              name n (t.as_name ()[0]);
              n.dir.clear ();
              dr << info << "perhaps it should be declared as being in the "
                 << "source tree: " << n << "@./ ?";
            }
          }

          break;
        }
      case target_decl::real:
        {
          // If we had a location, maybe it would make sense to mention this
          // case.
          //
          break;
        }
      }

      if (verb < 4)
        dr << info << "re-run with --verbose=4 for more information";
    }

    return nullptr;
  }

  recipe
  apply_impl (action a,
              target& t,
              const pair<const string, reference_wrapper<const rule>>& m)
  {
    const scope& bs (t.base_scope ());

    // Apply rules in project environment.
    //
    auto_project_env penv;
    if (const scope* rs = bs.root_scope ())
      penv = auto_project_env (*rs);

    const rule& ru (m.second);
    match_extra& me (t[a].match_extra);

    auto df = make_diag_frame (
      [a, &t, &m](const diag_record& dr)
      {
        if (verb != 0)
          dr << info << "while applying rule " << m.first << " to "
             << diag_do (a, t);
      });

    auto* f ((a.outer ()
              ? t.ctx.current_outer_oif
              : t.ctx.current_inner_oif)->adhoc_apply);

    auto* ar (f == nullptr ? nullptr : dynamic_cast<const adhoc_rule*> (&ru));

    recipe re (ar != nullptr ? f (*ar, a, t, me) : ru.apply (a, t, me));

    me.free (); // Note: cur_options are still in use.
    assert (me.cur_options != 0); // Match options cannot be 0 after apply().
    me.cur_options_.store (me.cur_options, memory_order_relaxed);
    return re;
  }

  static void
  apply_posthoc_impl (
    action a, target& t,
    const pair<const string, reference_wrapper<const rule>>& m,
    context::posthoc_target& pt)
  {
    const scope& bs (t.base_scope ());

    // Apply rules in project environment.
    //
    auto_project_env penv;
    if (const scope* rs = bs.root_scope ())
      penv = auto_project_env (*rs);

    const rule& ru (m.second);
    match_extra& me (t[a].match_extra);
    me.posthoc_prerequisite_targets = &pt.prerequisite_targets;

    auto df = make_diag_frame (
      [a, &t, &m](const diag_record& dr)
      {
        if (verb != 0)
          dr << info << "while applying rule " << m.first << " to "
             << diag_do (a, t) << " for post hoc prerequisites";
      });

    // Note: for now no adhoc_apply_posthoc().
    //
    ru.apply_posthoc (a, t, me);
  }

  static void
  reapply_impl (action a,
                target& t,
                const pair<const string, reference_wrapper<const rule>>& m)
  {
    const scope& bs (t.base_scope ());

    // Reapply rules in project environment.
    //
    auto_project_env penv;
    if (const scope* rs = bs.root_scope ())
      penv = auto_project_env (*rs);

    const rule& ru (m.second);
    match_extra& me (t[a].match_extra);
    // Note: me.posthoc_prerequisite_targets carried over.

    auto df = make_diag_frame (
      [a, &t, &m](const diag_record& dr)
      {
        if (verb != 0)
          dr << info << "while reapplying rule " << m.first << " to "
             << diag_do (a, t);
      });

    // Note: for now no adhoc_reapply().
    //
    ru.reapply (a, t, me);
    assert (me.cur_options != 0); // Match options cannot be 0 after reapply().
    me.cur_options_.store (me.cur_options, memory_order_relaxed);
  }

  // If anything goes wrong, set target state to failed and return nullopt.
  // Otherwise return the pointer to the new posthoc_target entry if any post
  // hoc prerequisites were present or NULL otherwise. Note that the returned
  // entry is stable (because we use a list) and should only be accessed
  // during the match phase if the holding the target lock.
  //
  // Note: must be called while holding target_lock.
  //
  static optional<context::posthoc_target*>
  match_posthoc (action a, target& t)
  {
    // The plan is to, while holding the lock, search and collect all the post
    // hoc prerequisited and add an entry to context::current_posthoc_targets.
    // The actual matching happens as post-pass in the meta-operation's match
    // function.
    //
    // While it may seem like we could do matching here by unlocking (or
    // unstacking) the lock for this target, that will only work for simple
    // cases. In particular, consider:
    //
    // lib{foo}: ...
    // lib{plug}: ... lib{foo}
    // libs{foo}: libs{plug}: include = posthoc
    //
    // The chain we will end up with:
    //
    // lib{foo}->libs{foo}=>libs{plug}->lib{foo}
    //
    // This will trip up the cycle detection for group lib{foo}, not for
    // libs{foo}.
    //
    // In the end, matching (and execution) "inline" (i.e., as we match/
    // execute the corresponding target) appears to be unworkable in the
    // face of cycles.
    //
    // Note also that this delayed match also helps with allowing the rule to
    // adjust match options of post hoc prerequisites without needing the
    // rematch support (see match_extra::posthoc_prerequisites).
    //
    // @@ Anything we need to do for group members (see through)? Feels quite
    //    far-fetched.
    //
    using posthoc_target = context::posthoc_target;
    using posthoc_prerequisite_target = posthoc_target::prerequisite_target;

    vector<posthoc_prerequisite_target> pts;
    try
    {
      for (const prerequisite& p: group_prerequisites (t))
      {
        // Note that we have to ignore any operation-specific values for
        // non-posthoc prerequisites. See include_impl() for details.
        //
        lookup l;
        if (include (a, t, p, &l) == include_type::posthoc)
        {
          if (l)
          {
            const string& v (cast<string> (l));

            // The only valid values are true and false and the latter would
            // have been translated to include_type::exclude.
            //
            if (v != "true")
            {
              fail << "unrecognized " << *l.var << " variable value "
                   << "'" << v << "' specified for prerequisite " << p;
            }
          }

          pts.push_back (
            posthoc_prerequisite_target {
              &search (t, p), // May fail.
              match_extra::all_options});
        }
      }
    }
    catch (const failed&)
    {
      t.state[a].state = target_state::failed;
      return nullopt;
    }

    if (!pts.empty ())
    {
      context& ctx (t.ctx);

      mlock l (ctx.current_posthoc_targets_mutex);
      ctx.current_posthoc_targets_collected.push_back (
        posthoc_target {a, t, move (pts)});
      return &ctx.current_posthoc_targets_collected.back (); // Stable.
    }

    return nullptr;
  }

  // If step is true then perform only one step of the match/apply sequence.
  //
  // If try_match is true, then indicate whether there is a rule match with
  // the first half of the result.
  //
  static pair<bool, target_state>
  match_impl_impl (target_lock& l,
                   uint64_t options,
                   bool step = false,
                   bool try_match = false)
  {
    // With regards to options, the semantics that we need to achieve for each
    // target::offeset_*:
    //
    // tried      -- nothing to do (no match)
    // touched    -- set to new_options
    // matched    -- add to new_options
    // applied    -- reapply if any new options
    // executed   -- check and fail if any new options
    // busy       -- postpone until *_complete() call
    //
    // Note that if options is 0 (see resolve_{members,group}_impl()), then
    // all this can be skipped.

    assert (l.target != nullptr);

    action a (l.action);
    target& t (*l.target);
    target::opstate& s (t[a]);

    try
    {
      // Intercept and handle matching an ad hoc group member.
      //
      if (t.adhoc_group_member ())
      {
        // It feels natural to "convert" this call to the one for the group,
        // including the try_match part. Semantically, we want to achieve the
        // following:
        //
        // [try_]match (a, g);
        // match_recipe (l, group_recipe);
        //
        // Currently, ad hoc group members cannot have options. An alternative
        // semantics could be to call the goup's rule to translate member
        // options to group options and then (re)match the group with that.
        // The implementation of this semantics could look like this:
        //
        // 1. Lock the group.
        // 2. If not already offset_matched, do one step to get the rule.
        // 3. Call the rule to translate options.
        // 4. Continue matching the group passing the translated options.
        // 5. Keep track of member options in member's cur_options to handle
        //    member rematches (if already offset_{applied,executed}).
        //
        // Note: see also similar semantics but for explicit groups in
        // adhoc-rule-*.cxx.

        assert (!step && options == match_extra::all_options);

        const target& g (*t.group);

        // What should we do with options? After some rumination it fells most
        // natural to treat options for the group and for its ad hoc member as
        // the same entity ... or not.
        //
        auto df = make_diag_frame (
          [a, &t](const diag_record& dr)
          {
            if (verb != 0)
              dr << info << "while matching group rule to " << diag_do (a, t);
          });

        pair<bool, target_state> r (
          match_impl (a, g, 0 /* options */, 0, nullptr, try_match));

        if (r.first)
        {
          if (r.second != target_state::failed)
          {
            // Note: in particular, passing all_options makes sure we will
            // never re-lock this member if already applied/executed.
            //
            match_inc_dependents (a, g);
            match_recipe (l, group_recipe, match_extra::all_options);

            // Note: no need to call match_posthoc() since an ad hoc member
            // has no own prerequisites and the group's ones will be matched
            // by the group.
          }
          else
          {
            // Similar to catch(failed) below.
            //
            s.state = target_state::failed;
            l.offset = target::offset_applied;

            // Make sure we don't relock a failed target.
            //
            match_extra& me (s.match_extra);
            me.cur_options = match_extra::all_options;
            me.cur_options_.store (me.cur_options, memory_order_relaxed);
          }
        }
        else
          l.offset = target::offset_tried;

        return r; // Group state (must be consistent with matched_state()).
      }

      // Continue from where the target has been left off.
      //
      switch (l.offset)
      {
      case target::offset_tried:
        {
          if (try_match)
            return make_pair (false, target_state::unknown);

          // To issue diagnostics ...
        }
        // Fall through.
      case target::offset_touched:
        {
          // Match.
          //

          // Clear the rule-specific variables, resolved targets list, and the
          // auxiliary data storage before calling match(). The rule is free
          // to modify these in its match() (provided that it matches) in
          // order to, for example, convey some information to apply().
          //
          clear_target (a, t);

          const rule_match* r (
            match_rule_impl (a, t, options, nullptr, try_match));

          assert (l.offset != target::offset_tried); // Should have failed.

          if (r == nullptr) // Not found (try_match == true).
          {
            l.offset = target::offset_tried;
            return make_pair (false, target_state::unknown);
          }

          set_rule (l, r);
          l.offset = target::offset_matched;

          if (step)
            // Note: s.state is still undetermined.
            return make_pair (true, target_state::unknown);

          // Otherwise ...
        }
        // Fall through.
      case target::offset_matched:
        {
          // Add any new options.
          //
          s.match_extra.new_options |= options;

          // Apply.
          //
          set_recipe (l, apply_impl (a, t, *s.rule));
          l.offset = target::offset_applied;

          if (t.has_group_prerequisites ()) // Ok since already matched.
          {
            if (optional<context::posthoc_target*> p = match_posthoc (a, t))
            {
              if (*p != nullptr)
              {
                // It would have been more elegant to do this before calling
                // apply_impl() and then expose the post hoc prerequisites to
                // apply(). The problem is the group may not be resolved until
                // the call to apply(). And so we resort to the separate
                // apply_posthoc() function.
                //
                apply_posthoc_impl (a, t, *s.rule, **p);
              }
            }
            else
              s.state = target_state::failed;
          }

          break;
        }
      case target::offset_applied:
        {
          // Reapply if any new options.
          //
          match_extra& me (s.match_extra);
          me.new_options = options & ~me.cur_options; // Clear existing.
          assert (me.new_options != 0); // Otherwise should not have locked.

          // Feels like this can only be a logic bug since to end up with a
          // subset of options requires a rule (see match_extra for details).
          //
          assert (s.rule != nullptr);

          reapply_impl (a, t, *s.rule);
          break;
        }
      case target::offset_executed:
        {
          // Diagnose new options after execute.
          //
          match_extra& me (s.match_extra);
          assert ((me.cur_options & options) != options); // Otherwise no lock.

          fail << "change of match options after " << diag_do (a, t)
               << " has been executed" <<
            info << "executed options 0x" << hex << me.cur_options <<
            info << "requested options 0x" << hex << options << endf;
        }
      default:
        assert (false);
      }
    }
    catch (const failed&)
    {
      s.state = target_state::failed;
      l.offset = target::offset_applied;

      // Make sure we don't relock a failed target.
      //
      match_extra& me (s.match_extra);
      me.cur_options = match_extra::all_options;
      me.cur_options_.store (me.cur_options, memory_order_relaxed);
    }

    if (s.state == target_state::failed)
    {
      // As a sanity measure clear the target data since it can be incomplete
      // or invalid (mark()/unmark() should give you some ideas).
      //
      clear_target (a, t);
    }

    return make_pair (true, s.state);
  }

  // If try_match is true, then indicate whether there is a rule match with
  // the first half of the result.
  //
  pair<bool, target_state>
  match_impl (action a, const target& ct,
              uint64_t options,
              size_t start_count, atomic_count* task_count,
              bool try_match)
  {
    // If we are blocking then work our own queue one task at a time. The
    // logic here is that we may have already queued other tasks before this
    // one and there is nothing bad (except a potentially deep stack trace)
    // about working through them while we wait. On the other hand, we want
    // to continue as soon as the lock is available in order not to nest
    // things unnecessarily.
    //
    // That's what we used to do but that proved to be too deadlock-prone. For
    // example, we may end up popping the last task which needs a lock that we
    // are already holding. A fuzzy feeling is that we need to look for tasks
    // (compare their task_counts?) that we can safely work on (though we will
    // need to watch out for indirections). So perhaps it's just better to keep
    // it simple and create a few extra threads.
    //
    target_lock l (
      lock_impl (a,
                 ct,
                 task_count == nullptr
                 ? optional<scheduler::work_queue> (scheduler::work_none)
                 : nullopt,
                 options));

    if (l.target != nullptr)
    {
      if (try_match && l.offset == target::offset_tried)
        return make_pair (false, target_state::unknown);

      if (task_count == nullptr)
        return match_impl_impl (l, options, false /* step */, try_match);

      // Pass "disassembled" lock since the scheduler queue doesn't support
      // task destruction.
      //
      target_lock::data ld (l.release ());

      // Also pass our diagnostics and lock stacks (this is safe since we
      // expect the caller to wait for completion before unwinding its stack).
      //
      // Note: pack captures and arguments a bit to reduce the storage space
      // requrements.
      //
      bool first (ld.first);

      if (ct.ctx.sched->async (
            start_count,
            *task_count,
            [a, try_match, first] (const diag_frame* ds,
                                   const target_lock* ls,
                                   target& t, size_t offset,
                                   uint64_t options)
            {
              // Switch to caller's diag and lock stacks.
              //
              diag_frame::stack_guard dsg (ds);
              target_lock::stack_guard lsg (ls);

              try
              {
                phase_lock pl (t.ctx, run_phase::match); // Throws.
                {
                  // Note: target_lock must be unlocked within the match phase.
                  //
                  target_lock l {a, &t, offset, first}; // Reassemble.
                  match_impl_impl (l, options, false /* step */, try_match);
                }
              }
              catch (const failed&) {} // Phase lock failure.
            },
            diag_frame::stack (),
            target_lock::stack (),
            ref (*ld.target), ld.offset,
            options))
        return make_pair (true, target_state::postponed); // Queued.

      // Matched synchronously, fall through.
    }
    else
    {
      // Already applied, executed, or busy.
      //
      if (l.offset >= target::offset_busy)
        return make_pair (true, target_state::busy);

      // Fall through.
    }

    return ct.try_matched_state (a, false);
  }

  void
  match_only_sync (action a, const target& t, uint64_t options)
  {
    assert (t.ctx.phase == run_phase::match);

    target_lock l (lock_impl (a, t, scheduler::work_none, options));

    if (l.target != nullptr)
    {
      if (l.offset != target::offset_matched)
      {
        if (match_impl_impl (l,
                             options,
                             true /* step */).second == target_state::failed)
          throw failed ();
      }
      else
      {
        // If the target is already matched, then we need to add any new
        // options but not call apply() (thus cannot use match_impl_impl()).
        //
        (*l.target)[a].match_extra.new_options |= options;
      }
    }
  }

  // Note: lock is a reference to avoid the stacking overhead.
  //
  static group_view
  resolve_members_impl (action a, const target& g, target_lock&& l)
  {
    assert (a.inner ());

    // Note that we will be unlocked if the target is already applied.
    //
    group_view r;

    // Continue from where the target has been left off.
    //
    switch (l.offset)
    {
    case target::offset_touched:
    case target::offset_tried:
      {
        // Match (locked).
        //
        if (match_impl_impl (l,
                             0 /* options */,
                             true /* step */).second == target_state::failed)
          throw failed ();

        if ((r = g.group_members (a)).members != nullptr)
          break;

        // To apply ...
      }
      // Fall through.
    case target::offset_matched:
      {
        // Apply (locked).
        //
        pair<bool, target_state> s (
          match_impl_impl (l, 0 /* options */, true /* step */));

        if (s.second == target_state::failed)
          throw failed ();

        if ((r = g.group_members (a)).members != nullptr)
        {
          // Doing match without execute messes up our target_count. There
          // doesn't seem to be a clean way to solve this. Well, just always
          // executing if we've done the match would have been clean but quite
          // heavy-handed (it would be especially surprising if otherwise
          // there is nothing else to do, which can happen, for example,
          // during update-for-test when there are no tests to run).
          //
          // So our solution is as follows:
          //
          // 1. Keep track both of the targets that ended up in this situation
          //    (the target::resolve_counted flag) as well as their total
          //    count (the context::resolve_count member). Only do this if
          //    set_recipe() (called by match_impl()) would have incremented
          //    target_count.
          //
          // 2. If we happen to execute such a target (common case), then
          //    clear the flag and decrement the count.
          //
          // 3. When it's time to assert that target_count==0 (i.e., all the
          //    matched targets have been executed), check if resolve_count is
          //    0. If it's not, then find every target with the flag set,
          //    pretend-execute it, and decrement both counts. See
          //    perform_execute() for further details on this step.
          //
          if (s.second != target_state::unchanged)
          {
            target::opstate& s (l.target->state[a]); // Inner.

            if (!s.recipe_group_action)
            {
              s.resolve_counted = true;
              g.ctx.resolve_count.fetch_add (1, memory_order_relaxed);
            }
          }
          break;
        }

        // Unlock and to execute ...
        //
        l.unlock ();
      }
      // Fall through.
    case target::offset_applied:
      {
        // Execute (unlocked).
        //
        // Note that we use execute_direct_sync() rather than execute_sync()
        // here to sidestep the dependents count logic. In this context, this
        // is by definition the first attempt to execute this rule (otherwise
        // we would have already known the members list) and we really do need
        // to execute it now.
        //
        // Note that while it might be tempting to decrement resolve_count
        // here, there is no guarantee that we were the ones who have matched
        // this target.
        //
        {
          phase_switch ps (g.ctx, run_phase::execute);
          execute_direct_sync (a, g);
        }

        r = g.group_members (a);
        break;
      }
    }

    return r;
  }

  group_view
  resolve_members (action a, const target& g)
  {
    group_view r;

    if (a.outer ())
      a = a.inner_action ();

    // We can be called during execute though everything should have been
    // already resolved.
    //
    switch (g.ctx.phase)
    {
    case run_phase::match:
      {
        // Grab a target lock to make sure the group state is synchronized.
        //
        target_lock l (lock_impl (a, g, scheduler::work_none));
        r = g.group_members (a);

        // If the group members are alrealy known or there is nothing else
        // we can do, then unlock and return.
        //
        if (r.members == nullptr && l.offset != target::offset_executed)
          r = resolve_members_impl (a, g, move (l));

        break;
      }
    case run_phase::execute: r = g.group_members (a); break;
    case run_phase::load:    assert (false);
    }

    return r;
  }

  // Note: lock is a reference to avoid the stacking overhead.
  //
  void
  resolve_group_impl (target_lock&& l)
  {
    assert (l.action.inner ());

    pair<bool, target_state> r (
      match_impl_impl (l,
                       0 /* options */,
                       true /* step */,
                       true /* try_match */));

    l.unlock ();

    if (r.first && r.second == target_state::failed)
      throw failed ();
  }

  template <typename R, typename S>
  static void
  match_prerequisite_range (action a, target& t,
                            R&& r,
                            const S& ms,
                            const scope* s,
                            bool search_only)
  {
    auto& pts (t.prerequisite_targets[a]);

    size_t i (pts.size ()); // Index of the first to be added.

    // Avoid duplicating fsdir{} that may have already been injected by
    // inject_fsdir() (in which case it is expected to be first).
    //
    const target* dir (nullptr);
    if (i != 0)
    {
      const prerequisite_target& pt (pts.front ());

      if (pt.target != nullptr && pt.adhoc () && pt.target->is_a<fsdir> ())
        dir = pt.target;
    }

    // Start asynchronous matching of prerequisites. Wait with unlocked phase
    // to allow phase switching.
    //
    wait_guard wg (
      search_only
      ? wait_guard ()
      : wait_guard (t.ctx, t.ctx.count_busy (), t[a].task_count, true));

    for (auto&& p: forward<R> (r))
    {
      // Ignore excluded.
      //
      include_type pi (include (a, t, p));

      if (!pi)
        continue;

      prerequisite_target pt (ms
                              ? ms (a, t, p, pi)
                              : prerequisite_target (&search (t, p), pi));

      if (pt.target == nullptr ||
          pt.target == dir     ||
          (s != nullptr && !pt.target->in (*s)))
        continue;

      if (!search_only)
        match_async (a, *pt.target, t.ctx.count_busy (), t[a].task_count);

      pts.push_back (move (pt));
    }

    if (search_only)
      return;

    wg.wait ();

    // Finish matching all the targets that we have started.
    //
    for (size_t n (pts.size ()); i != n; ++i)
    {
      const target& pt (*pts[i]);
      match_complete (a, pt);
    }
  }

  void
  match_prerequisites (action a, target& t,
                       const match_search& ms,
                       const scope* s,
                       bool search_only)
  {
    match_prerequisite_range (a, t,
                              group_prerequisites (t),
                              ms,
                              s,
                              search_only);
  }

  void
  match_prerequisite_members (action a, target& t,
                              const match_search_member& msm,
                              const scope* s,
                              bool search_only)
  {
    match_prerequisite_range (a, t,
                              group_prerequisite_members (a, t),
                              msm,
                              s,
                              search_only);
  }

  void
  match_members (action a, const target& t, const target* const* ts, size_t n)
  {
    // Pretty much identical to match_prerequisite_range() except we don't
    // search.
    //
    wait_guard wg (t.ctx, t.ctx.count_busy (), t[a].task_count, true);

    for (size_t i (0); i != n; ++i)
    {
      const target* m (ts[i]);

      if (m == nullptr || marked (m))
        continue;

      match_async (a, *m, t.ctx.count_busy (), t[a].task_count);
    }

    wg.wait ();

    // Finish matching all the targets that we have started.
    //
    for (size_t i (0); i != n; ++i)
    {
      const target* m (ts[i]);

      if (m == nullptr || marked (m))
        continue;

      match_complete (a, *m);
    }
  }

  void
  match_members (action a,
                 const target& t,
                 prerequisite_targets& ts,
                 size_t s,
                 pair<uintptr_t, uintptr_t> imv)
  {
    size_t n (ts.size ());

    wait_guard wg (t.ctx, t.ctx.count_busy (), t[a].task_count, true);

    for (size_t i (s); i != n; ++i)
    {
      const prerequisite_target& pt (ts[i]);
      const target* m (pt.target);

      if (m == nullptr ||
          marked (m)   ||
          (imv.first != 0 && (pt.include & imv.first) != imv.second))
        continue;

      match_async (a, *m, t.ctx.count_busy (), t[a].task_count);
    }

    wg.wait ();

    for (size_t i (s); i != n; ++i)
    {
      const prerequisite_target& pt (ts[i]);
      const target* m (pt.target);

      if (m == nullptr ||
          marked (m)   ||
          (imv.first != 0 && (pt.include & imv.first) != imv.second))
        continue;

      match_complete (a, *m);
    }
  }

  const fsdir*
  inject_fsdir_impl (target& t, bool prereq, bool parent)
  {
    tracer trace ("inject_fsdir");

    // If t is a directory (name is empty), say foo/bar/, then t is bar and
    // its parent directory is foo/.
    //
    const dir_path& d (parent && t.name.empty () ? t.dir.directory () : t.dir);

    const scope& bs (t.ctx.scopes.find_out (d));
    const scope* rs (bs.root_scope ());

    // If root scope is NULL, then this can mean that we are out of any
    // project or if the directory is in src_root. In both cases we don't
    // inject anything unless explicitly requested.
    //
    // Note that we also used to bail out if this is the root of the
    // project. But that proved not to be such a great idea in case of
    // subprojects (e.g., tests/).
    //
    const fsdir* r (nullptr);

    if (rs != nullptr && !d.sub (rs->src_path ()))
    {
      l6 ([&]{trace << d << " for " << t;});

      // Target is in the out tree, so out directory is empty.
      //
      r = &search<fsdir> (t, d, dir_path (), string (), nullptr, nullptr);
    }
    else if (prereq)
    {
      // See if one was mentioned explicitly.
      //
      for (const prerequisite& p: group_prerequisites (t))
      {
        if (p.is_a<fsdir> ())
        {
          const target& pt (search (t, p));

          if (pt.dir == d)
          {
            r = &pt.as<fsdir> ();
            break;
          }
        }
      }
    }

    return r;
  }

  const fsdir*
  inject_fsdir (action a, target& t, bool match, bool prereq, bool parent)
  {
    auto& pts (t.prerequisite_targets[a]);

    assert (!prereq || pts.empty ()); // This prerequisite target must be first.

    const fsdir* r (inject_fsdir_impl (t, prereq, parent));

    if (r != nullptr)
    {
      if (match)
        match_sync (a, *r);

      // Make it ad hoc so that it doesn't end up in prerequisite_targets
      // after execution.
      //
      pts.emplace_back (r, include_type::adhoc);
    }

    return r;
  }

  const fsdir*
  inject_fsdir_direct (action a, target& t, bool prereq, bool parent)
  {
    auto& pts (t.prerequisite_targets[a]);

    assert (!prereq || pts.empty ()); // This prerequisite target must be first.

    const fsdir* r (inject_fsdir_impl (t, prereq, parent));

    if (r != nullptr)
    {
      match_direct_sync (a, *r);
      pts.emplace_back (r, include_type::adhoc);
    }

    return r;
  }

  // Execute the specified recipe (if any) and the scope operation callbacks
  // (if any/applicable) then merge and return the resulting target state.
  //
  static target_state
  execute_recipe (action a, target& t, const recipe& r)
  {
    target_state ts (target_state::unchanged);

    try
    {
      auto df = make_diag_frame (
        [a, &t](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while " << diag_doing (a, t);
        });

      // If this is a dir{} target, see if we have any operation callbacks
      // in the corresponding scope.
      //
      const dir* op_t (t.is_a<dir> ());
      const scope* op_s (nullptr);

      using op_iterator = scope::operation_callback_map::const_iterator;
      pair<op_iterator, op_iterator> op_p;

      if (op_t != nullptr)
      {
        op_s = &t.ctx.scopes.find_out (t.dir); // Always out.

        if (op_s->out_path () == t.dir && !op_s->operation_callbacks.empty ())
        {
          op_p = op_s->operation_callbacks.equal_range (a);

          if (op_p.first == op_p.second)
            op_s = nullptr; // Ignore.
        }
        else
          op_s = nullptr; // Ignore.
      }

      if (r != nullptr || op_s != nullptr)
      {
        const scope& bs (t.base_scope ());

        // Execute recipe/callbacks in project environment.
        //
        auto_project_env penv;
        if (const scope* rs = bs.root_scope ())
          penv = auto_project_env (*rs);

        // Pre operations.
        //
        // Note that here we assume the dir{} target cannot be part of a group
        // and as a result we (a) don't try to avoid calling post callbacks in
        // case of a group failure and (b) merge the pre and post states with
        // the group state.
        //
        if (op_s != nullptr)
        {
          for (auto i (op_p.first); i != op_p.second; ++i)
            if (const auto& f = i->second.pre)
              ts |= f (a, *op_s, *op_t);
        }

        // Recipe.
        //
        if (r != nullptr)
          ts |= r (a, t);

        // Post operations.
        //
        if (op_s != nullptr)
        {
          for (auto i (op_p.first); i != op_p.second; ++i)
            if (const auto& f = i->second.post)
              ts |= f (a, *op_s, *op_t);
        }
      }

      // See the recipe documentation for details on what's going on here.
      // Note that if the result is group, then the group's state can be
      // failed.
      //
      switch (t[a].state = ts)
      {
      case target_state::changed:
      case target_state::unchanged:
        break;
      case target_state::postponed:
        // Keep the target state postponed (see group_action() for details)
        // but translate the result from postponed to unchanged, similar to
        // executed_state_impl().
        //
        ts = target_state::unchanged;
        break;
      case target_state::group:
        ts = (*t.group)[a].state;
        break;
      default:
        assert (false);
      }
    }
    catch (const failed&)
    {
      ts = t[a].state = target_state::failed;
    }

    return ts;
  }

  static inline const char*
  update_backlink_name (backlink_mode m, bool to_dir)
  {
    using mode = backlink_mode;

    const char* r (nullptr);
    switch (m)
    {
    case mode::link:
    case mode::symbolic:  r = verb >= 3 ? "ln -sf" : verb >= 2 ? "ln -s" : "ln"; break;
    case mode::hard:      r = verb >= 3 ? "ln -f" : "ln"; break;
    case mode::copy:
    case mode::overwrite: r = to_dir ? "cp -r" : "cp"; break;
    }
    return r;
  }

  void
  update_backlink (const file& f, const path& l, bool changed, backlink_mode m)
  {
    const path& p (f.path ());
    dir_path d (l.directory ());

    // At low verbosity levels we print the command if the target changed or
    // the link does not exist (we also treat errors as "not exist" and let
    // the link update code below handle it).
    //
    // Note that in the changed case we print it even if the link is not
    // actually updated to signal to the user that the updated out target is
    // now available in src.
    //
    if (verb == 1 || verb == 2)
    {
      if (changed || !butl::entry_exists (l,
                                          false /* follow_symlinks */,
                                          true  /* ignore_errors */))
      {
        const char* c (update_backlink_name (m, l.to_directory ()));

        // Note: 'ln foo/ bar/' means a different thing (and below).
        //
        if (verb == 2)
          text << c << ' ' << p.string () << ' ' << l.string ();
        else
          print_diag (c, f, d);
      }
    }

    // What if there is no such subdirectory in src (some like to stash their
    // executables in bin/ or some such). The easiest is probably just to
    // create it even though we won't be cleaning it up.
    //
    if (!exists (d))
      mkdir_p (d, 2 /* verbosity */);

    update_backlink (f.ctx, p, l, m);
  }

  void
  update_backlink (context& ctx,
                   const path& p, const path& l, bool changed, backlink_mode m)
  {
    // As above but with a slightly different diagnostics.

    dir_path d (l.directory ());

    if (verb == 1 || verb == 2)
    {
      if (changed || !butl::entry_exists (l,
                                          false /* follow_symlinks */,
                                          true  /* ignore_errors */))
      {
        const char* c (update_backlink_name (m, l.to_directory ()));

        // Note: 'ln foo/ bar/' means a different thing (and above) so strip
        // trailing directory separator (but keep as path for relative).
        //
        if (verb >= 2)
          text << c << ' ' << p.string () << ' ' << l.string ();
        else
          print_diag (c,
                      p.to_directory () ? path (p.string ()) : p,
                      d);
      }
    }

    if (!exists (d))
      mkdir_p (d, 2 /* verbosity */);

    update_backlink (ctx, p, l, m);
  }

  static inline void
  try_rmbacklink (const path& l,
                  backlink_mode m,
                  bool ie /* ignore_errors */= false)
  {
    // Note that this function should not be called in the dry-run mode.
    //
    // See also clean_backlink() below.

    using mode = backlink_mode;

    if (l.to_directory ())
    {
      switch (m)
      {
      case mode::link:
      case mode::symbolic:
      case mode::hard:      try_rmsymlink (l, true /* directory */, ie); break;
      case mode::copy:      try_rmdir_r   (path_cast<dir_path> (l), ie); break;
      case mode::overwrite:                                              break;
      }
    }
    else
    {
      // try_rmfile() should work for symbolic and hard file links.
      //
      switch (m)
      {
      case mode::link:
      case mode::symbolic:
      case mode::hard:
      case mode::copy:      try_rmfile (l, ie); break;
      case mode::overwrite:                     break;
      }
    }
  }

  void
  update_backlink (context& ctx,
                   const path& p, const path& l, backlink_mode om,
                   uint16_t verbosity)
  {
    assert (verbosity >= 2);

    using mode = backlink_mode;

    bool d (l.to_directory ());
    mode m (om); // Keep original mode.

    auto print = [&p, &l, &m, verbosity, d] ()
    {
      if (verb >= verbosity)
      {
        text << update_backlink_name (m, d) << ' ' << p.string () << ' '
             << l.string ();
      }
    };

    // Note that none of mk*() or cp*() functions that we use here handle
    // the dry-run mode.
    //
    if (!ctx.dry_run)
    try
    {
      try
      {
        // Normally will be there.
        //
        try_rmbacklink (l, m);

        // Skip (ad hoc) targets that don't exist.
        //
        if (!(d ? dir_exists (p) : file_exists (p)))
          return;

        switch (m)
        {
        case mode::link:
          if (!d)
          {
            mkanylink (p, l, false /* copy */);
            break;
          }
          // Directory hardlinks are not widely supported so for them we will
          // only try the symlink.
          //
          // Fall through.

        case mode::symbolic:  mksymlink  (p, l, d);  break;
        case mode::hard:
          {
            // The target can be a symlink (or a symlink chain) with a
            // relative target that, unless the (final) symlink and the
            // hardlink are in the same directory, will result in a dangling
            // link.
            //
            mkhardlink (followsymlink (p), l, d);
            break;
          }
        case mode::copy:
        case mode::overwrite:
          {
            if (d)
            {
              // Currently, for a directory, we do a "copy-link": we make the
              // target directory and then link each entry. (For now this is
              // only used to "link" a Windows DLL assembly with only files
              // inside. We also have to use hard links; see the relevant
              // comment in cc/link-rule for details. Maybe we can invent a
              // special Windows-only "assembly link" for this).
              //
              dir_path fr (path_cast<dir_path> (p));
              dir_path to (path_cast<dir_path> (l));

              try_mkdir (to);

              for (const auto& de: dir_iterator (fr, dir_iterator::no_follow))
              {
                path f (fr / de.path ());
                path t (to / de.path ());

                update_backlink (ctx, f, t, mode::hard, verb_never);
              }
            }
            else
              cpfile (p, l, (cpflags::overwrite_content |
                             cpflags::copy_timestamps));

            break;
          }
        }
      }
      catch (system_error& e)
      {
        // Translate to mkanylink()-like failure.
        //
        entry_type t (entry_type::unknown);
        switch (m)
        {
        case mode::link:
        case mode::symbolic:  t = entry_type::symlink;  break;
        case mode::hard:      t = entry_type::other;    break;
        case mode::copy:
        case mode::overwrite: t = entry_type::regular;  break;
        }

        throw pair<entry_type, system_error> (t, move (e));
      }
    }
    catch (const pair<entry_type, system_error>& e)
    {
      const char* w (e.first == entry_type::regular ? "copy"     :
                     e.first == entry_type::symlink ? "symlink"  :
                     e.first == entry_type::other   ? "hardlink" : nullptr);
      print ();
      fail << "unable to make " << w << ' ' << l << ": " << e.second;
    }

    print ();
  }

  void
  clean_backlink (context& ctx,
                  const path& l, uint16_t v /*verbosity*/, backlink_mode m)
  {
    // Like try_rmbacklink() but with diagnostics and error handling.
    //
    // Note that here the dry-run mode is handled by the filesystem functions.

    // Note that if we ever need to support level 1 for some reason, maybe
    // consider showing the target, for example, `unlink exe{hello} <- dir/`?
    //
    assert (v >= 2);

    using mode = backlink_mode;

    if (l.to_directory ())
    {
      switch (m)
      {
      case mode::link:
      case mode::symbolic:
      case mode::hard:  rmsymlink (ctx, l, true /* directory */, v);     break;
      case mode::copy:  rmdir_r (ctx, path_cast<dir_path> (l), true, v); break;
      case mode::overwrite:                                              break;
      }
    }
    else
    {
      // remfile() should work for symbolic and hard file links.
      //
      switch (m)
      {
      case mode::link:
      case mode::symbolic:
      case mode::hard:
      case mode::copy:      rmfile (ctx, l, v); break;
      case mode::overwrite:                     break;
      }
    }
  }

  // If target/link path are syntactically to a directory, then the backlink
  // is assumed to be to a directory, otherwise -- to a file.
  //
  struct backlink: auto_rm<path>
  {
    using path_type = build2::path;
    using target_type = build2::target;

    reference_wrapper<const path_type> target;
    backlink_mode                      mode;

    // Ad hoc group-specific information for diagnostics (see below).
    //
    const target_type*                 member = nullptr;
    bool                               print = true;

    backlink (const path_type& t, path_type&& l, backlink_mode m, bool active)
        : auto_rm<path_type> (move (l), active), target (t), mode (m)
    {
      assert (t.to_directory () == path.to_directory ());
    }

    ~backlink ()
    {
      if (active)
      {
        try_rmbacklink (path, mode, true /* ignore_errors */);
        active = false;
      }
    }

    backlink (backlink&&) = default;
    backlink& operator= (backlink&&) = default;
  };

  // Normally (i.e., on sane platforms that don't have things like PDBs, etc)
  // there will be just one or two backlinks so optimize for that.
  //
  using backlinks = small_vector<backlink, 2>;

  static optional<pair<backlink_mode, bool>>
  backlink_test (const target& t, const lookup& l, optional<backlink_mode> gm)
  {
    using mode = backlink_mode;

    const names& ns (cast<names> (l));

    if (ns.size () != 1 && ns.size () != 2)
    {
      fail << "invalid backlink variable value '" << ns << "' "
           << "specified for target " << t;
    }

    optional<mode> m;
    for (;;) // Breakout loop.
    {
      const name& n (ns.front ());

      if (n.simple ())
      {
        const string& v (n.value);

        if      (v == "true")      {m = mode::link;      break;}
        else if (v == "symbolic")  {m = mode::symbolic;  break;}
        else if (v == "hard")      {m = mode::hard;      break;}
        else if (v == "copy")      {m = mode::copy;      break;}
        else if (v == "overwrite") {m = mode::overwrite; break;}
        else if (v == "false")     {                     break;}
        else if (v == "group")     {if ((m = gm))        break;}
      }

      fail << "invalid backlink variable value mode component '" << n << "' "
           << "specified for target " << t << endf;
    }

    bool np (false); // "not print"
    if (ns.size () == 2)
    {
      const name& n (ns.back ());

      if (n.simple () && (n.value == "true" || (np = (n.value == "false"))))
        ;
      else
        fail << "invalid backlink variable value print component '" << n
             << "' specified for target " << t;
    }

    return m ? optional<pair<mode, bool>> (make_pair (*m, !np)) : nullopt;
  }

  static optional<backlink_mode>
  backlink_test (action a, target& t)
  {
    using mode = backlink_mode;

    context& ctx (t.ctx);

    // Note: the order of these checks is from the least to most expensive.

    // Only for plain update/clean.
    //
    if (a.outer () || (a != perform_update_id && a != perform_clean_id))
      return nullopt;

    // Only targets in the out tree can be backlinked.
    //
    if (!t.out.empty ())
      return nullopt;

    // Only file-based targets or groups containing file-based targets can be
    // backlinked. Note that we don't do the "file-based" check of the latter
    // case here since they can still be execluded. So instead we are prepared
    // to handle the empty backlinks list.
    //
    // @@ Potentially members could only be resolved in execute. I guess we
    //    don't support backlink for such groups at the moment.
    //
    if (!t.is_a<file> () && t.group_members (a).members == nullptr)
      return nullopt;

    // Neither an out-of-project nor in-src configuration can be forwarded.
    //
    const scope& bs (t.base_scope ());
    const scope* rs (bs.root_scope ());
    if (rs == nullptr || bs.src_path () == bs.out_path ())
      return nullopt;

    // Only for forwarded configurations.
    //
    if (!cast_false<bool> (rs->vars[ctx.var_forwarded]))
      return nullopt;

    lookup l (t.state[a][ctx.var_backlink]);

    // If not found, check for some defaults in the global scope (this does
    // not happen automatically since target type/pattern-specific lookup
    // stops at the project boundary).
    //
    if (!l.defined ())
      l = ctx.global_scope.lookup (*ctx.var_backlink, t.key ());

    optional<pair<mode, bool>> r (l ? backlink_test (t, l, nullopt) : nullopt);

    if (r && !r->second)
      fail << "backlink variable value print component cannot be false "
           << "for primary target " << t;

    return r ? optional<mode> (r->first) : nullopt;
  }

  static backlinks
  backlink_collect (action a, target& t, backlink_mode m)
  {
    using mode = backlink_mode;

    context& ctx (t.ctx);
    const scope& s (t.base_scope ());

    backlinks bls;
    auto add = [&bls, &s] (const path& p,
                           mode m,
                           const target* mt = nullptr,
                           bool print = true)
    {
      bls.emplace_back (p,
                        s.src_path () / p.leaf (s.out_path ()),
                        m,
                        !s.ctx.dry_run /* active */);

      if (mt != nullptr)
      {
        backlink& bl (bls.back ());
        bl.member = mt;
        bl.print = print;
      }
    };

    // Check for a custom backlink mode for this member. If none, then
    // inherit the one from the group (so if the user asked to copy
    // .exe, we will also copy .pdb).
    //
    // Note that we want to avoid group or tt/patter-spec lookup. And
    // since this is an ad hoc member (which means it was either declared
    // in the buildfile or added by the rule), we assume that the value,
    // if any, will be set as a target or rule-specific variable.
    //
    auto member_mode = [a, m, &ctx] (const target& mt)
      -> optional<pair<mode, bool>>
    {
      lookup l (mt.state[a].vars[ctx.var_backlink]);

      if (!l)
        l = mt.vars[ctx.var_backlink];

      return l ? backlink_test (mt, l, m) : make_pair (m, true);
    };

    // @@ Currently we don't handle the following cases:
    //
    // 1. File-based explicit groups.
    //
    // 2. Ad hoc subgroups in explicit groups.
    //
    // Note: see also the corresponding code in backlink_update_post().
    //
    if (file* f = t.is_a<file> ())
    {
      // First the target itself.
      //
      add (f->path (), m, f, true); // Note: always printed.

      // Then ad hoc group file/fsdir members, if any.
      //
      for (const target* mt (t.adhoc_member);
           mt != nullptr;
           mt = mt->adhoc_member)
      {
        const path* p (nullptr);

        if (const file* f = mt->is_a<file> ())
        {
          p = &f->path ();

          if (p->empty ()) // The "trust me, it's somewhere" case.
            p = nullptr;
        }
        else if (const fsdir* d = mt->is_a<fsdir> ())
          p = &d->dir;

        if (p != nullptr)
        {
          if (optional<pair<mode, bool>> m = member_mode (*mt))
            add (*p, m->first, mt, m->second);
        }
      }
    }
    else
    {
      // Explicit group.
      //
      group_view gv (t.group_members (a));
      assert (gv.members != nullptr);

      for (size_t i (0); i != gv.count; ++i)
      {
        if (const target* mt = gv.members[i])
        {
          if (const file* f = mt->is_a<file> ())
          {
            if (optional<pair<mode, bool>> m = member_mode (*mt))
              add (f->path (), m->first);
          }
        }
      }
    }

    return bls;
  }

  static inline backlinks
  backlink_update_pre (action a, target& t, backlink_mode m)
  {
    return backlink_collect (a, t, m);
  }

  static void
  backlink_update_post (target& t, target_state ts,
                        backlink_mode m, backlinks& bls)
  {
    if (ts == target_state::failed)
      return; // Let auto rm clean things up.

    context& ctx (t.ctx);

    file* ft (t.is_a<file> ());

    if (ft != nullptr && bls.size () == 1)
    {
      // Single file-based target.
      //
      const backlink& bl (bls.front ());

      update_backlink (*ft,
                       bl.path,
                       ts == target_state::changed,
                       bl.mode);
    }
    else
    {
      // Explicit or ad hoc group.
      //
      // What we have below is a custom variant of update_backlink(file).
      //
      dir_path d (bls.front ().path.directory ());

      // First print the verbosity level 1 diagnostics. Level 2 and higher are
      // handled by the update_backlink() calls below.
      //
      if (verb == 1)
      {
        bool changed (ts == target_state::changed);

        if (!changed)
        {
          for (const backlink& bl: bls)
          {
            changed = !butl::entry_exists (bl.path,
                                           false /* follow_symlinks */,
                                           true  /* ignore_errors */);
            if (changed)
              break;
          }
        }

        if (changed)
        {
          const char* c (update_backlink_name (m, false /* to_dir */));

          // For explicit groups we only print the group target. For ad hoc
          // groups we print all the members except those explicitly excluded.
          //
          if (ft == nullptr)
            print_diag (c, t, d);
          else
          {
            vector<target_key> tks;
            tks.reserve (bls.size ());

            for (const backlink& bl: bls)
              if (bl.print)
                tks.push_back (bl.member->key ());

            print_diag (c, move (tks), d);
          }
        }
      }

      if (!exists (d))
        mkdir_p (d, 2 /* verbosity */);

      // Make backlinks.
      //
      for (const backlink& bl: bls)
        update_backlink (ctx, bl.target, bl.path, bl.mode, 2 /* verbosity */);
    }

    // Cancel removal.
    //
    if (!ctx.dry_run)
    {
      for (backlink& bl: bls)
        bl.cancel ();
    }
  }

  static void
  backlink_clean_pre (action a, target& t, backlink_mode m)
  {
    backlinks bls (backlink_collect (a, t, m));

    for (auto b (bls.begin ()), i (b); i != bls.end (); ++i)
    {
      // Printing anything at level 1 will probably just add more noise.
      //
      backlink& bl (*i);
      bl.cancel ();
      clean_backlink (t.ctx, bl.path, i == b ? 2 : 3 /* verbosity */, bl.mode);
    }
  }

  static target_state
  execute_impl (action a, target& t)
  {
    context& ctx (t.ctx);

    target::opstate& s (t[a]);

    assert (s.task_count.load (memory_order_consume) == t.ctx.count_busy ()
            && s.state == target_state::unknown);

    target_state ts;
    try
    {
      // Handle target backlinking to forwarded configurations.
      //
      // Note that this function will never be called if the recipe is noop
      // which is ok since such targets are probably not interesting for
      // backlinking.
      //
      // Note also that for group members (both ad hoc and non) backlinking
      // is handled when updating/cleaning the group.
      //
      backlinks bls;
      optional<backlink_mode> blm;

      if (t.group == nullptr) // Matched so must be already resolved.
      {
        blm = backlink_test (a, t);

        if (blm)
        {
          if (a == perform_update_id)
          {
            bls = backlink_update_pre (a, t, *blm);
            if (bls.empty ())
              blm = nullopt;
          }
          else
            backlink_clean_pre (a, t, *blm);
        }
      }

      // Note: see similar code in set_rule_trace() for match.
      //
      if (ctx.trace_execute != nullptr && trace_target (t, *ctx.trace_execute))
      {
        diag_record dr (info);

        dr << diag_doing (a, t);

        if (s.rule != nullptr)
        {
          const rule& r (s.rule->second);

          if (const adhoc_rule* ar = dynamic_cast<const adhoc_rule*> (&r))
          {
            dr << info (ar->loc);

            if (ar->pattern != nullptr)
              dr << "using ad hoc pattern rule ";
            else
              dr << "using ad hoc recipe ";
          }
          else
            dr << info << "using rule ";

          dr << s.rule->first;
        }
        else
          dr << info << "using directly-assigned recipe";
      }

      ts = execute_recipe (a, t, s.recipe);

      if (blm)
      {
        if (a == perform_update_id)
          backlink_update_post (t, ts, *blm, bls);
      }
    }
    catch (const failed&)
    {
      // If we could not backlink the target, then the best way to signal the
      // failure seems to be to mark the target as failed.
      //
      ts = s.state = target_state::failed;
    }

    // Clear the recipe to release any associated memory. Note that
    // s.recipe_group_action may be used further (see, for example,
    // group_state()) and should retain its value.
    //
    if (!s.recipe_keep)
      s.recipe = nullptr;

    // Decrement the target count (see set_recipe() for details).
    //
    // Note that here we cannot rely on s.state being group because of the
    // postponment logic (see excute_recipe() for details).
    //
    if (a.inner () && !s.recipe_group_action)
    {
      // See resolve_members_impl() for background.
      //
      if (s.resolve_counted)
      {
        s.resolve_counted = false;
        ctx.resolve_count.fetch_sub (1, memory_order_relaxed);
      }

      ctx.target_count.fetch_sub (1, memory_order_relaxed);
    }

    // Decrement the task count (to count_executed) and wake up any threads
    // that might be waiting for this target.
    //
    size_t tc (s.task_count.fetch_sub (
                 target::offset_busy - target::offset_executed,
                 memory_order_release));
    assert (tc == ctx.count_busy ());
    ctx.sched->resume (s.task_count);

    return ts;
  }

  target_state
  execute_impl (action a,
                const target& ct,
                size_t start_count,
                atomic_count* task_count)
  {
    // NOTE: see also pretend_execute lambda in perform_execute().

    target& t (const_cast<target&> (ct)); // MT-aware.
    target::opstate& s (t[a]);

    context& ctx (t.ctx);

    // Update dependency counts and make sure they are not skew.
    //
    size_t gd (ctx.dependency_count.fetch_sub (1, memory_order_relaxed));
    size_t td (s.dependents.fetch_sub (1, memory_order_release));
    assert (td != 0 && gd != 0);

    // Handle the "last" execution mode.
    //
    // This gets interesting when we consider interaction with groups. It seem
    // to make sense to treat group members as dependents of the group, so,
    // for example, if we try to clean the group via three of its members,
    // only the last attempt will actually execute the clean. This means that
    // when we match a group member, inside we should also match the group in
    // order to increment the dependents count. This seems to be a natural
    // requirement: if we are delegating to the group, we need to find a
    // recipe for it, just like we would for a prerequisite.
    //
    // Note that we are also going to treat the group state as postponed.
    // This is not a mistake: until we execute the recipe, we want to keep
    // returning postponed. And once the recipe is executed, it will reset the
    // state to group (see group_action()). To put it another way, the
    // execution of this member is postponed, not of the group.
    //
    // Note also that the target execution is postponed with regards to this
    // thread. For other threads the state will still be unknown (until they
    // try to execute it).
    //
    if (ctx.current_mode == execution_mode::last && --td != 0)
      return target_state::postponed;

    // Try to atomically change applied to busy.
    //
    size_t tc (ctx.count_applied ());

    size_t exec (ctx.count_executed ());
    size_t busy (ctx.count_busy ());

    optional<target_state> r;
    if (s.task_count.compare_exchange_strong (
          tc,
          busy,
          memory_order_acq_rel,  // Synchronize on success.
          memory_order_acquire)) // Synchronize on failure.
    {
      // Handle the noop recipe.
      //
      if (s.state == target_state::unchanged)
      {
        // There could still be scope operations.
        //
        r = t.is_a<dir> ()
          ? execute_recipe (a, t, nullptr /* recipe */)
          : s.state;

        s.task_count.store (exec, memory_order_release);
        ctx.sched->resume (s.task_count);
      }
      else
      {
        if (task_count == nullptr)
          r = execute_impl (a, t);
        else
        {
          // Pass our diagnostics stack (this is safe since we expect the
          // caller to wait for completion before unwinding its diag stack).
          //
          if (ctx.sched->async (start_count,
                                *task_count,
                                [a] (const diag_frame* ds, target& t)
                                {
                                  diag_frame::stack_guard dsg (ds);
                                  execute_impl (a, t);
                                },
                                diag_frame::stack (),
                                ref (t)))
            return target_state::unknown; // Queued.

          // Executed synchronously, fall through.
        }
      }
    }
    else
    {
      // Either busy or already executed.
      //
      if (tc >= busy) return target_state::busy;
      else            assert (tc == exec);
    }

    return r ? *r : t.executed_state (a, false /* fail */);
  }

  target_state
  execute_direct_impl (action a,
                       const target& ct,
                       size_t start_count,
                       atomic_count* task_count)
  {
    context& ctx (ct.ctx);

    target& t (const_cast<target&> (ct)); // MT-aware.
    target::opstate& s (t[a]);

    // Similar logic to execute_impl() above.
    //
    size_t tc (ctx.count_applied ());

    size_t exec (ctx.count_executed ());
    size_t busy (ctx.count_busy ());

    optional<target_state> r;
    if (s.task_count.compare_exchange_strong (
          tc,
          busy,
          memory_order_acq_rel,  // Synchronize on success.
          memory_order_acquire)) // Synchronize on failure.
    {
      if (s.state == target_state::unknown)
      {
        if (task_count == nullptr)
          r = execute_impl (a, t);
        else
        {
          if (ctx.sched->async (start_count,
                                *task_count,
                                [a] (const diag_frame* ds, target& t)
                                {
                                  diag_frame::stack_guard dsg (ds);
                                  execute_impl (a, t);
                                },
                                diag_frame::stack (),
                                ref (t)))
            return target_state::unknown; // Queued.

          // Executed synchronously, fall through.
        }
      }
      else
      {
        assert (s.state == target_state::unchanged ||
                s.state == target_state::failed);

        r = s.state == target_state::unchanged && t.is_a<dir> ()
          ? execute_recipe (a, t, nullptr /* recipe */)
          : s.state;

        s.task_count.store (exec, memory_order_release);
        ctx.sched->resume (s.task_count);
      }
    }
    else
    {
      // Either busy or already executed.
      //
      if (tc >= busy) return target_state::busy;
      else            assert (tc == exec);
    }

    return r ? *r : t.executed_state (a, false /* fail */);
  }

  bool
  update_during_match (tracer& trace, action a, const target& t, timestamp ts)
  {
    // NOTE: see also clean_during_match() if changing anything here.

    assert (a == perform_update_id);

    // Note: this function is used to make sure header dependencies are up to
    // date (and which is where it originated).
    //
    // There would normally be a lot of headers for every source file (think
    // all the system headers) and just calling execute_direct_sync() on all
    // of them can get expensive. At the same time, most of these headers are
    // existing files that we will never be updating (again, system headers,
    // for example) and the rule that will match them is the fallback
    // file_rule. That rule has an optimization: it returns noop_recipe (which
    // causes the target state to be automatically set to unchanged) if the
    // file is known to be up to date. So we do the update "smartly".
    //
    // Also, now that we do header pre-generation by default, there is a good
    // chance the header has already been updated. So we also detect that and
    // avoid switching the phase.
    //
    const path_target* pt (t.is_a<path_target> ());

    if (pt == nullptr)
      ts = timestamp_unknown;

    target_state os (t.matched_state (a));

    if (os == target_state::unchanged)
    {
      if (ts == timestamp_unknown)
        return false;
      else
      {
        // We expect the timestamp to be known (i.e., existing file).
        //
        timestamp mt (pt->mtime ());
        assert (mt != timestamp_unknown);
        return mt > ts;
      }
    }
    else
    {
      // We only want to return true if our call to execute() actually caused
      // an update. In particular, the target could already have been in
      // target_state::changed because of the dynamic dependency extraction
      // run for some other target.
      //
      target_state ns;
      if (os != target_state::changed)
      {
        phase_switch ps (t.ctx, run_phase::execute);
        ns = execute_direct_sync (a, t);
      }
      else
        ns = os;

      if (ns != os && ns != target_state::unchanged)
      {
        l6 ([&]{trace << "updated " << t
                      << "; old state " << os
                      << "; new state " << ns;});
        return true;
      }
      else
        return ts != timestamp_unknown ? pt->newer (ts, ns) : false;
    }
  }

  bool
  update_during_match_prerequisites (tracer& trace,
                                     action a, target& t,
                                     uintptr_t mask)
  {
    // NOTE: see also clean_during_match_prerequisites() if changing anything
    //       here.

    assert (a == perform_update_id);

    prerequisite_targets& pts (t.prerequisite_targets[a]);

    // On the first pass detect and handle unchanged tragets. Note that we
    // have to do it in a separate pass since we cannot call matched_state()
    // once we've switched the phase.
    //
    size_t n (0);

    for (prerequisite_target& p: pts)
    {
      if (mask == 0 || (p.include & mask) != 0)
      {
        if (p.target != nullptr)
        {
          const target& pt (*p.target);

          target_state os (pt.matched_state (a));

          if (os != target_state::unchanged)
          {
            ++n;
            p.data = static_cast<uintptr_t> (os);
            continue;
          }
        }

        p.data = 0;
      }
    }

    // If all unchanged, we are done.
    //
    if (n == 0)
      return false;

    // Provide additional information on what's going on.
    //
    auto df = make_diag_frame (
      [&t](const diag_record& dr)
      {
        if (verb != 0)
          dr << info << "while updating during match prerequisites of "
             << "target " << t;
      });

    context& ctx (t.ctx);

    phase_switch ps (ctx, run_phase::execute);

    bool r (false);

    // @@ Maybe we should optimize for n == 1? Maybe we should just call
    //    smarter update_during_match() in this case?
    //
#if 0
    for (prerequisite_target& p: pts)
    {
      if ((mask == 0 || (p.include & mask) != 0) && p.data != 0)
      {
        const target& pt (*p.target);

        target_state os (static_cast<target_state> (p.data));
        target_state ns (execute_direct_sync (a, pt));

        if (ns != os && ns != target_state::unchanged)
        {
          l6 ([&]{trace << "updated " << pt
                        << "; old state " << os
                        << "; new state " << ns;});
          r = true;
        }

        p.data = 0;
      }
    }
#else

    // Start asynchronous execution of prerequisites. Similar logic to
    // straight_execute_members().
    //
    // Note that the target's task count is expected to be busy (since this
    // function is called during match). And there don't seem to be any
    // problems in using it for execute.
    //
    atomic_count& tc (t[a].task_count);

    size_t busy (ctx.count_busy ());

    wait_guard wg (ctx, busy, tc);

    for (prerequisite_target& p: pts)
    {
      if ((mask == 0 || (p.include & mask) != 0) && p.data != 0)
      {
        execute_direct_async (a, *p.target, busy, tc);
      }
    }

    wg.wait ();

    // Finish execution and process the result.
    //
    for (prerequisite_target& p: pts)
    {
      if ((mask == 0 || (p.include & mask) != 0) && p.data != 0)
      {
        const target& pt (*p.target);
        target_state ns (execute_complete (a, pt));
        target_state os (static_cast<target_state> (p.data));

        if (ns != os && ns != target_state::unchanged)
        {
          l6 ([&]{trace << "updated " << pt
                        << "; old state " << os
                        << "; new state " << ns;});
          r = true;
        }

        p.data = 0;
      }
    }
#endif

    return r;
  }

  bool
  clean_during_match (tracer& trace, action a, const target& t)
  {
    // Let's keep this as close to update_during_match() semantically as
    // possible until we see a clear reason to deviate.

    // We have a problem with fsdir{}: if the directory is not empty because
    // there are other targets that depend on it and we execute it here and
    // now, it will not remove the directory (because it's not yet empty) but
    // will cause the target to be in the executed state, which means that
    // when other targets try to execute it, it will be a noop and the
    // directory will be left behind.

    assert (a == perform_clean_id && !t.is_a<fsdir> ());

    target_state os (t.matched_state (a));

    if (os == target_state::unchanged)
      return false;
    else
    {
      target_state ns;
      if (os != target_state::changed)
      {
        phase_switch ps (t.ctx, run_phase::execute);
        ns = execute_direct_sync (a, t);
      }
      else
        ns = os;

      if (ns != os && ns != target_state::unchanged)
      {
        l6 ([&]{trace << "cleaned " << t
                      << "; old state " << os
                      << "; new state " << ns;});
        return true;
      }
      else
        return false;
    }
  }

  bool
  clean_during_match_prerequisites (tracer& trace,
                                    action a, target& t,
                                    uintptr_t mask)
  {
    // Let's keep this as close to update_during_match_prerequisites()
    // semantically as possible until we see a clear reason to deviate.
    //
    // Currently the only substantial change is the reverse iteration order.

    assert (a == perform_clean_id);

    prerequisite_targets& pts (t.prerequisite_targets[a]);

    // On the first pass detect and handle unchanged tragets. Note that we
    // have to do it in a separate pass since we cannot call matched_state()
    // once we've switched the phase.
    //
    size_t n (0);

    for (prerequisite_target& p: pts)
    {
      if (mask == 0 || (p.include & mask) != 0)
      {
        if (p.target != nullptr)
        {
          const target& pt (*p.target);

          assert (!pt.is_a<fsdir> ()); // See above.

          target_state os (pt.matched_state (a));

          if (os != target_state::unchanged)
          {
            ++n;
            p.data = static_cast<uintptr_t> (os);
            continue;
          }
        }

        p.data = 0;
      }
    }

    // If all unchanged, we are done.
    //
    if (n == 0)
      return false;

    // Provide additional information on what's going on.
    //
    auto df = make_diag_frame (
      [&t](const diag_record& dr)
      {
        if (verb != 0)
          dr << info << "while cleaning during match prerequisites of "
             << "target " << t;
      });

    context& ctx (t.ctx);

    phase_switch ps (ctx, run_phase::execute);

    bool r (false);

    // @@ Maybe we should optimize for n == 1? Maybe we should just call
    //    smarter clean_during_match() in this case?
    //
#if 0
    for (prerequisite_target& p: reverse_iterate (pts))
    {
      if ((mask == 0 || (p.include & mask) != 0) && p.data != 0)
      {
        const target& pt (*p.target);

        target_state os (static_cast<target_state> (p.data));
        target_state ns (execute_direct_sync (a, pt));

        if (ns != os && ns != target_state::unchanged)
        {
          l6 ([&]{trace << "cleaned " << pt
                        << "; old state " << os
                        << "; new state " << ns;});
          r = true;
        }

        p.data = 0;
      }
    }
#else

    // Start asynchronous execution of prerequisites. Similar logic to
    // straight_execute_members().
    //
    // Note that the target's task count is expected to be busy (since this
    // function is called during match). And there don't seem to be any
    // problems in using it for execute.
    //
    atomic_count& tc (t[a].task_count);

    size_t busy (ctx.count_busy ());

    wait_guard wg (ctx, busy, tc);

    for (prerequisite_target& p: reverse_iterate (pts))
    {
      if ((mask == 0 || (p.include & mask) != 0) && p.data != 0)
      {
        execute_direct_async (a, *p.target, busy, tc);
      }
    }

    wg.wait ();

    // Finish execution and process the result.
    //
    for (prerequisite_target& p: reverse_iterate (pts))
    {
      if ((mask == 0 || (p.include & mask) != 0) && p.data != 0)
      {
        const target& pt (*p.target);
        target_state ns (execute_complete (a, pt));
        target_state os (static_cast<target_state> (p.data));

        if (ns != os && ns != target_state::unchanged)
        {
          l6 ([&]{trace << "cleaned " << pt
                        << "; old state " << os
                        << "; new state " << ns;});
          r = true;
        }

        p.data = 0;
      }
    }
#endif

    return r;
  }

  action_targets
  update_during_load (const scope& bs,
                      const path& bf,
                      names ns,
                      const location& loc)
  {
    tracer trace ("update_during_load");

    context& ctx (bs.ctx);

    // The meta-operation should be known since we don't allow updating
    // during bootstrap.
    //
    assert (ctx.phase == run_phase::load &&
            ctx.current_mif != nullptr &&
            !ns.empty ());

    bool in_uctx (ctx.update_during_load_context == &ctx); // U-d-l context.

    l5 ([&]{trace (loc) << "updating"
                        << (in_uctx ? " in u-d-l context" : "")
                        << " targets " << ns
                        << " from " << bf;});

    // Resolve target names to targets (in this context).
    //
    action_targets ts;
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& n (*i), o;
      bool p (n.pair);
      const target& t (to_target (bs,
                                  move (n), move (p ? *++i : o),
                                  false /* in_recipe */,
                                  loc));

      if (p)
        fail (loc) << "source target " << t << " in update during load";

      // We cannot update aliases during load due to their special semantics
      // in the load-only mode (see below for details). Note that we don't
      // diagnose alias prerequisites.
      //
      if (t.is_a<alias> ())
        fail (loc) << "alias target " << t << " in update during load";

      ts.push_back (&t);
    }

    // The overall plan is pretty simple:
    //
    // 1. Switch the phase to match and match the target for upate.
    //
    // 2. Switch the phase to execute and update the target.
    //
    // There is just one complication: This could be an interrupting load,
    // meaning there could be other threads waiting for the match phase. But
    // that match phase could be for a different action. Specifically, it can
    // be for a different operation say, clean, but it can also be for a
    // different meta-operation, say configure or dist.
    //
    // So what we are going to do is use different approaches depending on the
    // action. If it is perform_update, then we will update directly in this
    // context. Otherwise, we create a special separate build context and
    // update the target there (similar to how we build build system modules).
    // The first approach will be fast, which is important since we want
    // perform_update to be as fast a possible. The second approach will be a
    // lot slower (we have to bootstrap the project, load the buildfile, etc)
    // but that doesn't matter since clean, configure, etc., are not executed
    // very often.
    //
    // Note also that we reasonably assume operations with update as a pre-
    // operation (e.g., test) will load everything during the pre-operation.
    // But if not, additional targets will automatically get updated via the
    // separate context.
    //
    // Finally, note that in case of an error while updating during load a
    // target, we may end up with slightly different diagnostics depending on
    // whether this is initial or interrupting load. In the former case we
    // won't get the info message for the main targets.
    //
    if (ctx.current_mif->id == perform_id)
    {
      // If the operation was not set, assume it's some special build mode
      // (like bpkg skeleton) that does not support update during load.
      //
      // Also check for the module context where we don't support update
      // during load.
      //
      if (ctx.current_on == 0 || ctx.module_context == &ctx)
      {
        const string& bm (cast<string> (ctx.global_scope["build.mode"]));

        // A build system driver that uses the normal build mode should
        // support update during load.
        //
        assert (bm != "normal");

        fail (loc) << "update during load is not supported in " << bm
                   << " build mode";
      }

      action oa (ctx.current_action ());

      l6 ([&]{trace << "called with known action " << oa;});

      // We assume we can build whatever needs to be loaded during update with
      // an outer operation with any incompatibilities (e.g., for/not-for
      // install) detected naturally. More precisely, if we ended up executing
      // first, it will be detected, but not the other way around. On the
      // other hand, some incompatibilities could be harmless (e.g.,
      // for-test). Maybe we should just let this case play out natually
      // (e.g., in the for-install case we won't be able to run an executable
      // already built for-install). But on yet another hand, why would
      // someone need to load during the build something that was built, say,
      // for test? It would be best to diagnose it and see what shakes out
      // but it's unclear how exactly to detect this.
      //
      if (oa.inner_action () == perform_update_id)
      {
        action a (perform_update_id); // Drop outer action.

        // Cutoff the existing diagnostics stack and push our own entry. It
        // feels like keeping the existing diagnostics stack will only add to
        // the noise (note: also cuts off otherwise duplicate diag frame in
        // case of uctx).
        //
        diag_frame::stack_guard diag_cutoff (nullptr);

        auto df = make_diag_frame (
          [&loc, &ts] (const diag_record& dr)
          {
            // Print first target for context.
            //
            dr << info (loc) << "while updating " << ts.front ().as<target> ()
               << " during load";
          });

        // If we are in the initial load then we use standard perform_match()
        // and perform_execute(). In particular, this will call operation
        // callbacks, setup progress, etc. Note that perform_match() and
        // perform_execute() have a lot of special update-during-load logic.
        // Note also that this pair can be called repeatedly to update
        // multiple targets during load.
        //
        // Otherwise, we are in the interrupting load, which means match is
        // already in progress and we should instead switch the phase.
        //
        if (ctx.phase_mutex.unlocked ()) // Initial load.
        {
          l6 ([&]{trace << "updating during initial load";});

          // It doesn't feel like a recursive update-during-load in the
          // initial load makes much sense (dir{} as update-during-load
          // target/prerequisite, which we disallowed).
          //
          // However, in the interrupting load update case, other rules may
          // continue with their work once we switch the phase to match. And
          // that can involve loading of additional buildfiles which may
          // contain update directives.
          //
          if (ctx.update_during_load != 0)
            fail (loc) << "recursive update during load";

          auto udlg = make_guard ([&ctx] () {ctx.update_during_load = 0;});
          ctx.update_during_load = 1; // Initial load.

          // Clear the match-only mode. While we ignore match-only by calling
          // perform_execute() unconditionally, the rules may still adjust
          // their logic based on this mode.
          //
          auto mog = make_guard ([&ctx, o = ctx.match_only] ()
                                 {
                                   ctx.match_only = o;
                                 });
          ctx.match_only = nullopt;

          // Note that we suppress all outcome diagnostics, even for failure
          // since the extra `info: failed to update <target>` is not very
          // useful (the diagnostics frame above explains what's going on).
          //
          // What should we do about the progress, keeping in mind that in the
          // interrupting load case below (which will be more common) we will
          // be piggybacking on the the same progress as update-during-match.
          // If we do the standard thing, then we will see the percentage that
          // will reach 100%, which will definitely look confusing since we
          // will then start another progress from 0%. After some meditation,
          // it felt most sensible to emulate the same progress as in
          // update-during-match, but with the "during load" instead of
          // "during match" diagnostics. This is all hacked in as special
          // cases in perform_match() and perform_execute().
          //
          // Note that we disable progress if updating in the u-d-l context
          // (see below for the rationale).
          //
          // Note: perfrom_match() omits verifying targets for u-d-l.
          //
          perform_match ({}, a, ts,
                         0 /* diag (none) */,
                         !in_uctx /* progress */);

          // Note: perform_execute() ignores the dry-run mode for u-d-l.
          // Note: perfrom_execute() omits handling posthoc targets for u-d-l.
          // Note: perfrom_execute() omits target count checks for u-d-l.
          //
          perform_execute ({}, a, ts,
                           0 /* diag (none) */,
                           !in_uctx /* progress */);
        }
        else // Interrupting load.
        {
          l6 ([&]{trace << "updating during interrupting load";});

          // This case can only get triggered during match, before the dry-run
          // mode is in effect (see perform_execute() for details).
          //
          assert (!ctx.dry_run);

          // We should never end up here from the initial load or while
          // updating in the u-d-l context (it should always be the initial
          // load, see below). The only possibility is dir{} as an update-
          // during-load target/prerequisite, which we disallowed.
          //
          if (in_uctx || ctx.update_during_load == 1)
            fail (loc) << "recursive update during load";

          // But it can be recursive interrupting load case and it can be
          // interleaving, so we must keep track of the balance.
          //
          auto udlg = make_guard (
            [&ctx] ()
            {
              // Note: runs in the serial load phase.
              //
              if (ctx.update_during_load == 2)
                ctx.update_during_load = 0;
              else
                --ctx.update_during_load;
            });

          if (ctx.update_during_load == 0)
            ctx.update_during_load = 2; // Interrupting load.
          else
            ++ctx.update_during_load;

          // This means there is a perform update action already in progress
          // in this context. So we are going to switch the phase and perform
          // direct match and update.
          //
          // Note that since neither match nor execute are serial phases, it
          // means other targets in this context can be matched and executed
          // in paralellel with us.
          //
          // A couple of nuances:
          //
          // - When we switch to the match phase, there is a partially loaded
          //   buildfile which may have already "advertised" its targets (for
          //   example, via the export stub) but haven't yet defined them. We
          //   deal with that in dir_search(), which feels like the only place
          //   we can load additional buildfiles during interrupting load.
          //
          // - While we ignore match-only by calling execute unconditionally,
          //   the rules may still adjust their logic based on this mode. In
          //   particular, alias_rule will only keep alias prerequisites in
          //   the load-only mode. It doesn't seem there is anything we can do
          //   except not allowing updating aliases during load (diagnosed
          //   above).
          //
          // - Posthoc prerequisites will not yet be updated and thus not
          //   usable during load. Again, there is nothing we can easily do
          //   about it. Note that the perform_execute() call recreates the
          //   the same semantics, for consistency.

          // Mark the queue so that we don't work any already enqueued tasks.
          // Failed that we may deadlock via dir_search().
          //
          scheduler::queue_mark qm (*ctx.sched);

          if (ts.size () == 1) // Common case.
          {
            const target& t (ts.front ().as<target> ());

            phase_switch mp (ctx, run_phase::match);
            if (match_sync (perform_update_id, t) != target_state::unchanged)
            {
              phase_switch ep (ctx, run_phase::execute);
              execute_sync (a, t);
            }
          }
          else
          {
            // Parallel match and execute.
            //
            target_state s (target_state::unknown);

            // Match.
            //
            phase_switch mp (ctx, run_phase::match);
            {
              // Start asynchronous matching of targets. Wait with unlocked
              // phase to allow phase switching.
              //
              atomic_count task_count (0);
              wait_guard wg (ctx, task_count, true);

              for (const auto& t: ts)
                match_async (a, t.as<target> (), 0, task_count);

              wg.wait ();

              // Finish matching all the targets that we have started.
              //
              for (const auto& t: ts)
                s |= match_complete (a, t.as<target> ());
            }

            // Execute.
            //
            if (s != target_state::unchanged)
            {
              phase_switch ep (ctx, run_phase::execute);

              // Start asynchronous execution of targets.
              //
              atomic_count task_count (0);
              wait_guard wg (ctx, task_count);

              for (const auto& t: ts)
                execute_async (a, t.as<target> (), 0, task_count);

              wg.wait ();

              // Finish executing all the targets that we have started.
              //
              for (const auto& t: ts)
                execute_complete (a, t.as<target> ());
            }
          }
        }

        // Record the targets in the updated_during_match map.
        //
        // Note that we do this even if we are in uctx since we load the
        // buildfile (see below) and thus may need this information in the
        // u-d-l context, not only in the main context.
        //
        for (size_t i (0); i != ts.size (); ++i)
        {
          const target* k (&ts[i].as<target> ());

          auto p (ctx.updated_during_load.emplace (k, k));

          assert (p.second || p.first->second == k);
        }

        l5 ([&]{trace (loc) << "end" << (in_uctx ? " in u-d-l context" : "");});

        return ts;
      }

      // Fall through to the separate context case.
    }

    // If we are here, then it means either the meta-operation is not perform
    // or the operation is not update. In both cases we have to update the
    // targets in a separate context where we can execute perform_update.
    //
    l6 ([&]{trace << "updating via u-d-l context";});

    // Note that there is no recursivity here since the u-d-l context will
    // only be executing perform_update, which means any recursive updates
    // during load should be handled by the above direct update logic.
    //
    assert (!in_uctx);

    // NOTE: similar to create_module_context()/update_in_module_context().

    // Create the context unless already exists.
    //
    // Note that this context is reset before every subsequent operation in
    // context::current_operation().
    //
    if (ctx.update_during_load_context == nullptr)
    {
      // Remap verbosity (see below for details). We also have to do this
      // while creating the context because the verbosity is set as a
      // variable on the global scope.
      //
      auto verbg = make_guard (
        [z = !silent && verb == 0 ? (verb = 1, true) : false] ()
        {
          if (z)
            verb = 0;
        });

      // Note: propagate module context both ways (see below for the other
      // direction).
      //
      ctx.update_during_load_context_storage.reset (
        new context (*ctx.sched,
                     *ctx.mutexes,
                     *ctx.fcache,
                     nullopt,                       /* match_only */
                     false,                         /* no_external_modules */
                     false,                         /* dry_run */
                     ctx.no_diag_buffer,
                     ctx.keep_going,
                     string ("update-during-load"), /* build_mode */
                     ctx.original_var_overrides,    /* cmd_vars */
                     false,                         /* cmd_vars_global_only */
                     context::reserves {0, 0},
                     ctx.module_context));          /* module_context */

      context& uctx (*(ctx.update_during_load_context =
                       ctx.update_during_load_context_storage.get ()));

      // Establish our exemplar (necessary for variable override propagation).
      //
      uctx.exemplar_context = &ctx;

      // Note: this is used to detect u-d-l context (plus the special build
      // mode above).
      //
      uctx.update_during_load_context = &uctx;

      // Setup the context to perform update. In a sense we have a long-
      // running perform meta-operation batch (indefinite, in fact, since we
      // never call the meta-operation's *_post() callbacks) in which we
      // periodically execute update operations.
      //
      // Note that we perform each build in a separate update operation since
      // we have to load buildfiles in-between and it matches how the standard
      // driver does it.
      //
      if (mo_perform.meta_operation_pre != nullptr)
        mo_perform.meta_operation_pre (uctx, {} /* parameters */, loc);

      uctx.current_meta_operation (mo_perform);

      if (mo_perform.operation_pre != nullptr)
        mo_perform.operation_pre (uctx, {} /* parameters */, update_id);
    }

    context& uctx (*ctx.update_during_load_context);

    // Copy over any new operation callbacks. If a callback implementation
    // does not wish to see u-d-l context's calls, it can filter them out
    // based on the passed context.
    //
    // Note that only the callbacks registered before we need to build the
    // u-d-l target will be in effect. Note also that we assume callbacks are
    // never unregistered or replaced and thus can avoid copying if sizes
    // match.
    //
    if (uctx.operation_callbacks.size () != ctx.operation_callbacks.size ())
      uctx.operation_callbacks = ctx.operation_callbacks;

    // Inherit module_libraries lock from the outer context.
    //
    uctx.modules_lock = ctx.modules_lock;

    action_targets uts;
    {
      // Clear current project's environment and "switch" to the u-d-l
      // context, including entering a scheduler sub-phase.
      //
      auto_thread_env penv (nullptr);
      context& ctx (uctx);
      scheduler::phase_guard pg (*ctx.sched);

      {
        // Cutoff the existing diagnostics stack and push our own entry.
        //
        diag_frame::stack_guard diag_cutoff (nullptr);

        auto df = make_diag_frame (
          [&loc, &ts] (const diag_record& dr)
          {
            dr << info (loc) << "while updating " << ts.front ().as<target> ()
               << " during load";
          });

        // New update operation.
        //
        assert (op_update.operation_pre == nullptr &&
                op_update.operation_post == nullptr);

        ctx.current_operation (op_update);

        action a (perform_update_id);

        // Un-tune the scheduler.
        //
        // Note that we can only do this if we are running serially because
        // otherwise we cannot guarantee the scheduler is idle (we could have
        // waiting threads from the outer context). This is fine for now since
        // the only two tuning level we use are serial and full concurrency.
        // (Turns out currently we don't really need this: we will always be
        // called during load or match phases and we always do parallel match;
        // but let's keep it in case things change. Actually, we may need it,
        // if the scheduler was started up in a tuned state, like in bpkg).
        //
        auto sched_tune (ctx.sched->serial ()
                         ? scheduler::tune_guard (*ctx.sched, 0)
                         : scheduler::tune_guard ());

        // Remap verbosity level 0 to 1 unless we were requested to be silent.
        // Failed that, we may have long periods of seemingly nothing
        // happening while we quietly update the target, which may look like
        // things have hung up.
        //
        // @@ CTX: modifying global verbosity level won't work if we have
        //         multiple top-level contexts running in parallel.
        //
        auto verbg = make_guard (
          [z = !silent && verb == 0 ? (verb = 1, true) : false] ()
          {
            if (z)
              verb = 0;
          });

        // Note that the above direct update logic would normally show
        // progress and which theoretically could clash with the progress of
        // what we are already doing. Practically, however, none of the
        // meta-operations other than perform currently show progress, except
        // for dist which shows it later, when copying the files. The perform
        // meta-operation with an operation other than update (e.g., test) may
        // show progress. But none of them do it during match (remember, we
        // may only end up here during the initial load or the match-
        // interrupting load). So we could show progress. But it will probably
        // look off: we will only show progress of updating u-d-l targets, not
        // of the main action we are performing. So for now we detect in the
        // above direct update logic that we are in the u-d-l context and
        // suppress progress.
        //
        // Note also that the scheduler does not support nested progress
        // monitors, which would happen if we are triggered via interrupting
        // load (e.g., clean).
        //
        // @@ On the other hand, long u-d-l without progress during
        //    configuration, clean, look really strange. Maybe if the
        //    first progress clearly indicated it is for "during load",
        //    then it will be ok?
        //
        //    BTW, we also suppress progress in direct update of targets
        //    below, and this stuff should be consistent.
        //
        //    Also, we may have multiple calls like this which are executed in
        //    seperate operation batches. So there will be multiple
        //    progresses? Fuzzy.
        //
        //    Perhaps the solution is to exclude long-to-update targets from
        //    configure and for clean, hope already up to date?
        //
        // See also update_in_module_context().

        // What we are going to do is simply load the buildfile. If it
        // contains the update directive, this will trigger the update of this
        // (and potentially further) targets. Note that it will be the initial
        // load in the above direct update logic. And if it doesn't contain
        // the update directive (e.g., we are called by a module), then we
        // will update them in the closing perform_match()/peform_execute()
        // below.
        //
        // Note also that we don't try to optimize things much here (like
        // checking if the buildfile is already loaded before setting things
        // up) since this is the slow path.
        //
        path abf (bf);
        try
        {
          if (bf.relative ())
            abf.complete ();

          abf.normalize ();
        }
        catch (const invalid_path&)
        {
          fail (loc) << "invalid buildfile path '" << bf.string () << "'";
        }

        // Load the project and buildfile in the u-d-l context.
        //
        const scope& ubs (exemplar_load (ctx, bs, move (abf), loc));

        // Translate targets to the new context.
        //
        for (const auto& t: ts)
        {
          uts.push_back (
            &to_target (ubs,
                        t.as<target> ().as_name (),
                        false /* in_recipe */,
                        loc));
        }

        // Call closing perform_match()/perform_execute() to perform all the
        // omitted parts (update posthoc, verify, etc). Note that we pass the
        // list of the (potentially) already updated targets so other than the
        // omitted parts, this could be a noop. (Or, it could be that the
        // buildfile doesn't contain the update directives, for example, if
        // this function is called by a module.)
        //
        // Note that it may seem better to work this logic into the above
        // calls of these function. Note, however, that while these calls will
        // execute exactly once per operation batch, the above calls may be
        // executed multiple time if the buildfile we are loading contains
        // more update during load directives.
        //
        {
          perform_match ({}, a, uts,
                         0 /* diag (none) */,
                         false /* progress */);

          perform_execute ({}, a, uts,
                           0 /* diag (none) */,
                           false /* progress */);
        }

        ctx.load_generation++;
      }
    }

    uctx.modules_lock = nullptr; // For good measure.

    // If a module context was created while updating during load, transfer it
    // to the main context.
    //
    if (uctx.module_context != ctx.module_context)
    {
      assert (uctx.module_context == uctx.module_context_storage->get ());
      ctx.module_context_storage = move (*uctx.module_context_storage);
      ctx.module_context = uctx.module_context;
    }

    // The targets (and their prerequisites) which we have updated in the
    // u-d-l context also exist in the main context and now their view of the
    // filesystem state (specifically, mtime) may be unsynchronized. So,
    // conceptually, it would seem correct to try to synchronize their states
    // (specifically, propagate the mtime from u-d-l to the main context).
    //
    // Practically, however, this appears unnecessary in some cases and
    // insufficient in others. Specifically, the only case that has the
    // potential to cause trouble due to this unsynchronization is
    // perform_clean (other actions tend not to care about mtime). And the
    // (so far) identified problematic scenarios are the following:
    //
    // 1. Clean during match. For example, a common prerequisite may have
    //    gotten cleaned between two updates during load.
    //
    // 2. Clean rule implementation makes decisions based on mtime (loaded or
    //    cached) during match or based on cached mtime during execute. For
    //    example, an implementation may return noop_recipe if it sees mtime
    //    is nonexistent while the target has been/will be updated in the
    //    u-d-l context.
    //
    // 3. Subsequent update operation bases decisions on the cached mtime
    //    (nonexistent) set by the preceding clean operation while the target
    //    was updated in the u-d-l context.
    //
    // To resolve (1), we would need to synchronize the state in both
    // directions on each update_during_load() call. While doable, clean
    // during match is very unusual and it seems more reasonable to just
    // declare it a limitation (see the clean_id documentation in action.hxx).
    //
    // To resolve (2), specifically the match case, after synchronizing the
    // state, we would have to also re-match the already matched rule to allow
    // it to make a different decision. This is quite hairy while in practice
    // we don't have any such rule implementations (vast majority of rules use
    // the standard perform_clean*() implementations).
    //
    // Finally, (3) does not appear to be an issue since for the update
    // operation, the u-d-l target will be updated in the main context, not in
    // the u-d-l context and thus will automatically synchronize the state.
    //
    // As a result, the current approach is to place some limitations on the
    // clean operation implementation (see the clean_id documentation in
    // action.hxx).
    //
    // Note also that if we do end up needing to synchronize the state, then
    // this can be done in two places: at the beginning and/or end of each
    // update_during_load() call (to synchronize the state within the same
    // main context operation) and in context::current_operation() (to do it
    // between main context operations), depending on what we need.

    // Record the target mapping in the updated_during_match map.
    //
    assert (ts.size () == uts.size ());

    for (size_t i (0); i != ts.size (); ++i)
    {
      const target* k (&ts[i].as<target> ());
      const target* v (&uts[i].as<target> ());

      auto p (ctx.updated_during_load.emplace (k, v));

      assert (p.second || p.first->second == v);
    }

    l5 ([&]{trace (loc) << "end via u-d-l context";});

    return uts;
  }

  static inline void
  blank_adhoc_member (const target*&)
  {
  }

  static inline void
  blank_adhoc_member (prerequisite_target& pt)
  {
    if (pt.adhoc ())
      pt.target = nullptr;
  }

  template <typename T>
  target_state
  straight_execute_members (context& ctx, action a, atomic_count& tc,
                            T ts[], size_t n, size_t p)
  {
    target_state r (target_state::unchanged);

    size_t busy (ctx.count_busy ());

    // Start asynchronous execution of prerequisites.
    //
    wait_guard wg (ctx, busy, tc);

    n += p;
    for (size_t i (p); i != n; ++i)
    {
      const target*& mt (ts[i]);

      if (mt == nullptr) // Skipped.
        continue;

      target_state s (execute_async (a, *mt, busy, tc));

      if (s == target_state::postponed)
      {
        r |= s;
        mt = nullptr;
      }
    }

    wg.wait ();

    // Now all the targets in prerequisite_targets must be either still busy
    // or executed and synchronized (and we have blanked out all the postponed
    // ones).
    //
    for (size_t i (p); i != n; ++i)
    {
      if (ts[i] == nullptr)
        continue;

      const target& mt (*ts[i]);
      r |= execute_complete (a, mt);

      blank_adhoc_member (ts[i]);
    }

    return r;
  }

  template <typename T>
  target_state
  reverse_execute_members (context& ctx, action a, atomic_count& tc,
                           T ts[], size_t n, size_t p)
  {
    // Pretty much as straight_execute_members() but in reverse order.
    //
    target_state r (target_state::unchanged);

    size_t busy (ctx.count_busy ());

    wait_guard wg (ctx, busy, tc);

    n = p - n;
    for (size_t i (p); i != n; )
    {
      const target*& mt (ts[--i]);

      if (mt == nullptr)
        continue;

      target_state s (execute_async (a, *mt, busy, tc));

      if (s == target_state::postponed)
      {
        r |= s;
        mt = nullptr;
      }
    }

    wg.wait ();

    for (size_t i (p); i != n; )
    {
      if (ts[--i] == nullptr)
        continue;

      const target& mt (*ts[i]);
      r |= execute_complete (a, mt);

      blank_adhoc_member (ts[i]);
    }

    return r;
  }

  // Instantiate only for what we need.
  //
  template LIBBUILD2_SYMEXPORT target_state
  straight_execute_members<const target*> (
    context&, action, atomic_count&, const target*[], size_t, size_t);

  template LIBBUILD2_SYMEXPORT target_state
  reverse_execute_members<const target*> (
    context&, action, atomic_count&, const target*[], size_t, size_t);

  template LIBBUILD2_SYMEXPORT target_state
  straight_execute_members<prerequisite_target> (
    context&, action, atomic_count&, prerequisite_target[], size_t, size_t);

  template LIBBUILD2_SYMEXPORT target_state
  reverse_execute_members<prerequisite_target> (
    context&, action, atomic_count&, prerequisite_target[], size_t, size_t);

  pair<optional<target_state>, const target*>
  execute_prerequisites (const target_type* tt,
                         action a, const target& t,
                         const timestamp& mt, const execute_filter& ef,
                         size_t n)
  {
    assert (a == perform_update_id);

    context& ctx (t.ctx);

    size_t busy (ctx.count_busy ());

    auto& pts (t.prerequisite_targets[a]);

    if (n == 0)
      n = pts.size ();

    // Pretty much as straight_execute_members() but hairier.
    //
    target_state rs (target_state::unchanged);

    wait_guard wg (ctx, busy, t[a].task_count);

    for (size_t i (0); i != n; ++i)
    {
      const target*& pt (pts[i]);

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
    const target* rt (nullptr);

    for (size_t i (0); i != n; ++i)
    {
      prerequisite_target& p (pts[i]);

      if (p == nullptr)
        continue;

      const target& pt (*p.target);
      target_state s (execute_complete (a, pt));
      rs |= s;

      // Should we compare the timestamp to this target's?
      //
      if (!e && (p.adhoc () || !ef || ef (pt, i)))
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
          // Otherwise we assume the prerequisite is newer if it was changed.
          //
          if (s == target_state::changed)
            e = true;
        }
      }

      if (p.adhoc ())
        p.target = nullptr; // Blank out.
      else if (tt != nullptr)
      {
        if (rt == nullptr && pt.is_a (*tt))
          rt = &pt;
      }
    }

    assert (tt == nullptr || rt != nullptr);

    return pair<optional<target_state>, const target*> (
      e ? optional<target_state> () : rs, rt);
  }

  pair<optional<target_state>, const target*>
  reverse_execute_prerequisites (const target_type* tt,
                                 action a, const target& t,
                                 const timestamp& mt, const execute_filter& ef,
                                 size_t n)
  {
    assert (a == perform_update_id);

    context& ctx (t.ctx);

    size_t busy (ctx.count_busy ());

    auto& pts (t.prerequisite_targets[a]);

    if (n == 0)
      n = pts.size ();

    // Pretty much as reverse_execute_members() but hairier.
    //
    target_state rs (target_state::unchanged);

    wait_guard wg (ctx, busy, t[a].task_count);

    for (size_t i (n); i != 0; )
    {
      const target*& pt (pts[--i]);

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
    const target* rt (nullptr);

    for (size_t i (n); i != 0; )
    {
      prerequisite_target& p (pts[--i]);

      if (p == nullptr)
        continue;

      const target& pt (*p.target);
      target_state s (execute_complete (a, pt));
      rs |= s;

      // Should we compare the timestamp to this target's?
      //
      if (!e && (p.adhoc () || !ef || ef (pt, i)))
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
          // Otherwise we assume the prerequisite is newer if it was changed.
          //
          if (s == target_state::changed)
            e = true;
        }
      }

      if (p.adhoc ())
        p.target = nullptr; // Blank out.
      else if (tt != nullptr)
      {
        // Note that here we need last.
        //
        if (pt.is_a (*tt))
          rt = &pt;
      }
    }

    assert (tt == nullptr || rt != nullptr);

    return pair<optional<target_state>, const target*> (
      e ? optional<target_state> () : rs, rt);
  }

  target_state
  noop_action (action a, const target& t)
  {
    error << "noop action triggered for " << diag_doing (a, t);
    assert (false); // We shouldn't be called (see set_recipe()).
    return target_state::unchanged;
  }

  target_state
  group_action (action a, const target& t)
  {
    context& ctx (t.ctx);

    // If the group is busy, we wait, similar to prerequisites.
    //
    const target& g (*t.group);

    // This is execute_sync(a, t, false) but that saves a call to
    // executed_state() (which we don't need).
    //
    target_state gs (execute_impl (a, g, 0, nullptr));

    if (gs == target_state::busy)
      ctx.sched->wait (ctx.count_executed (),
                      g[a].task_count,
                      scheduler::work_none);

    // Return target_state::group to signal to execute() that this target's
    // state comes from the group (which, BTW, can be failed).
    //
    // There is just one small problem: if the returned group state is
    // postponed, then this means the group hasn't been executed yet. And if
    // we return target_state::group, then this means any state queries (see
    // executed_state()) will be directed to the target which might still not
    // be executed or, worse, is being executed as we query.
    //
    // So in this case we return target_state::postponed (which will result in
    // the member being treated as unchanged). This is how it is done for
    // prerequisites and seeing that we've been acting as if the group is our
    // prerequisite, there is no reason to deviate (see the recipe return
    // value documentation for details).
    //
    return gs != target_state::postponed ? target_state::group : gs;
  }

  target_state
  default_action (action a, const target& t)
  {
    return execute_prerequisites (a, t);
  }

  static target_state
  clean_extra (context& ctx,
               const path& fp,
               const clean_extras& es,
               path& ep, bool& ed)
  {
    assert (!fp.empty ()); // Must be assigned.

    target_state er (target_state::unchanged);

    for (const char* e: es)
    {
      size_t n;
      if (e == nullptr || (n = strlen (e)) == 0)
        continue;

      path p;
      bool d;

      if (path::traits_type::absolute (e))
      {
        p = path (e);
        d = p.to_directory ();
      }
      else
      {
        if ((d = (e[n - 1] == '/')))
          --n;

        p = fp;
        for (; *e == '-'; ++e)
          p = p.base ();

        p.append (e, n);
      }

      target_state r (target_state::unchanged);

      if (d)
      {
        dir_path dp (path_cast<dir_path> (p));

        switch (rmdir_r (ctx, dp, true, 3))
        {
        case rmdir_status::success:
          {
            r = target_state::changed;
            break;
          }
        case rmdir_status::not_empty:
          {
            if (verb >= 3)
              info << dp << " is current working directory, not removing";
            break;
          }
        case rmdir_status::not_exist:
          break;
        }
      }
      else
      {
        if (rmfile (ctx, p, 3))
          r = target_state::changed;
      }

      if (r == target_state::changed && ep.empty ())
      {
        ed = d;
        ep = move (p);
      }

      er |= r;
    }

    return er;
  }

  target_state
  perform_clean_extra (action a, const file& ft,
                       const clean_extras& extras,
                       const clean_adhoc_extras& adhoc_extras,
                       bool show_adhoc)
  {
    context& ctx (ft.ctx);

    // Clean the extras first and don't print the commands at verbosity level
    // below 3. Note the first extra file/directory that actually got removed
    // for diagnostics below.
    //
    // Note that dry-run is taken care of by the filesystem functions.
    //
    target_state er (target_state::unchanged);
    bool ed (false);
    path ep;

    const path& fp (ft.path ());

    if (!fp.empty () && !extras.empty ())
      er |= clean_extra (ctx, fp, extras, ep, ed);

    target_state tr (target_state::unchanged);

    // Check if we were asked not to actually remove the files. The extras are
    // tricky: some of them, like depdb should definitely be removed. But
    // there could also be those that shouldn't. Currently we only use this
    // for auto-generated source code where the only extra file, if any, is
    // depdb so for now we treat them as "to remove" but in the future we may
    // need to have two lists.
    //
    bool clean (cast_true<bool> (ft[ctx.var_clean]));

    // Now clean the ad hoc group file members, if any.
    //
    // While at it, also collect the group target keys if we are showing
    // the members. But only those that exist (since we don't want to
    // print any diagnostics if none of them exist).
    //
    vector<target_key> tks;

    for (const target* m (ft.adhoc_member);
         m != nullptr;
         m = m->adhoc_member)
    {
      const file* mf (m->is_a<file> ());
      const path* mp (mf != nullptr ? &mf->path () : nullptr);

      if (mf == nullptr || mp->empty ())
        continue;

      if (!adhoc_extras.empty ())
      {
        auto i (find_if (adhoc_extras.begin (),
                         adhoc_extras.end (),
                         [mf] (const clean_adhoc_extra& e)
                         {
                           return mf->is_a (e.type);
                         }));

        if (i != adhoc_extras.end ())
          er |= clean_extra (ctx, *mp, i->extras, ep, ed);
      }

      if (!clean)
        continue;

      // Make this "primary target" for diagnostics/result purposes if the
      // primary target is unreal.
      //
      if (fp.empty ())
      {
        if (rmfile (*mp, *mf))
          tr = target_state::changed;
      }
      else
      {
        target_state r (rmfile (ctx, *mp, 3)
                        ? target_state::changed
                        : target_state::unchanged);

        if (r == target_state::changed)
        {
          if (show_adhoc && verb == 1)
            tks.push_back (mf->key ());
          else if (ep.empty ())
          {
            ep = *mp;
            er |= r;
          }
        }
      }
    }

    // Now clean the primary target and its prerequisited in the reverse order
    // of update: first remove the file, then clean the prerequisites.
    //
    if (clean && !fp.empty ())
    {
      if (show_adhoc && verb == 1 && !tks.empty ())
      {
        if (rmfile (fp, ft, 2 /* verbosity */))
          tks.insert (tks.begin (), ft.key ());

        print_diag ("rm", move (tks));
        tr = target_state::changed;
      }
      else
      {
        if (rmfile (fp, ft))
          tr = target_state::changed;
      }
    }

    // Update timestamp in case there are operations after us that could use
    // the information.
    //
    ft.mtime (timestamp_nonexistent);

    // We factor the result of removing the extra files into the target state.
    // While strictly speaking removing them doesn't change the target state,
    // if we don't do this, then we may end up removing the file but still
    // saying that everything is clean (e.g., if someone removes the target
    // file but leaves the extra laying around). That would be confusing.
    //
    // What would also be confusing is if we didn't print any commands in
    // this case.
    //
    if (tr != target_state::changed && er == target_state::changed)
    {
      if (verb > (ctx.current_diag_noise ? 0 : 1) && verb < 3)
      {
        if (verb >= 2)
        {
          if (ed)
            text << "rm -r " << path_cast<dir_path> (ep);
          else
            text << "rm " << ep;
        }
        else if (verb)
        {
          if (ed)
            print_diag ("rm -r", path_cast<dir_path> (ep));
          else
            print_diag ("rm", ep);
        }
      }
    }

    // Clean prerequisites.
    //
    tr |= reverse_execute_prerequisites (a, ft);

    tr |= er;
    return tr;
  }

  target_state
  perform_clean_group_extra (action a, const mtime_target& g,
                             const clean_extras& extras)
  {
    context& ctx (g.ctx);

    target_state er (target_state::unchanged);
    bool ed (false);
    path ep;

    if (!extras.empty ())
      er |= clean_extra (ctx, g.dir / path (g.name), extras, ep, ed);

    target_state tr (target_state::unchanged);

    if (cast_true<bool> (g[g.ctx.var_clean]))
    {
      for (group_view gv (g.group_members (a)); gv.count != 0; --gv.count)
      {
        if (const target* m = gv.members[gv.count - 1])
        {
          // Note that at the verbosity level 1 we don't show the removal of
          // each group member. This is consistent with what is normally shown
          // during update.
          //
          if (rmfile (m->as<file> ().path (), *m, 2 /* verbosity */))
            tr |= target_state::changed;
        }
      }

      if (tr == target_state::changed && verb == 1)
        print_diag ("rm", g);
    }

    g.mtime (timestamp_nonexistent);

    if (tr != target_state::changed && er == target_state::changed)
    {
      if (verb > (ctx.current_diag_noise ? 0 : 1) && verb < 3)
      {
        if (verb >= 2)
        {
          if (ed)
            text << "rm -r " << path_cast<dir_path> (ep);
          else
            text << "rm " << ep;
        }
        else if (verb)
        {
          if (ed)
            print_diag ("rm -r", path_cast<dir_path> (ep));
          else
            print_diag ("rm", ep);
        }
      }
    }

    tr |= reverse_execute_prerequisites (a, g);

    tr |= er;
    return tr;
  }

  target_state
  perform_clean (action a, const target& t)
  {
    const file& f (t.as<file> ());
    assert (!f.path ().empty ());
    return perform_clean_extra (a, f, {});
  }

  target_state
  perform_clean_depdb (action a, const target& t)
  {
    const file& f (t.as<file> ());
    assert (!f.path ().empty ());
    return perform_clean_extra (a, f, {".d"});
  }

  target_state
  perform_clean_group (action a, const target& t)
  {
    return perform_clean_group_extra (a, t.as<mtime_target> (), {});
  }

  target_state
  perform_clean_group_depdb (action a, const target& t)
  {
    path d;
    clean_extras extras;
    {
      group_view gv (t.group_members (a));
      if (gv.count != 0)
      {
        for (size_t i (0); i != gv.count; ++i)
        {
          if (const target* m = gv.members[i])
          {
            d = m->as<file> ().path () + ".d";
            break;
          }
        }

        assert (!d.empty ());
        extras.push_back (d.string ().c_str ());
      }
    }

    return perform_clean_group_extra (a, t.as<mtime_target> (), extras);
  }
}
