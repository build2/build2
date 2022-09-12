// file      : libbuild2/algorithm.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/algorithm.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/rule.hxx>
#include <libbuild2/file.hxx> // import()
#include <libbuild2/search.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite.hxx>

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
    assert (t.ctx.phase == run_phase::match);

    // If this is a project-qualified prerequisite, then this is import's
    // business.
    //
    if (pk.proj)
      return import (t.ctx, pk);

    if (const target* pt = pk.tk.type->search (t, pk))
      return *pt;

    return create_new_target (t.ctx, pk);
  }

  pair<target&, ulock>
  search_locked (const target& t, const prerequisite_key& pk)
  {
    assert (t.ctx.phase == run_phase::match && !pk.proj);

    if (const target* pt = pk.tk.type->search (t, pk))
      return {const_cast<target&> (*pt), ulock ()};

    return create_new_target_locked (t.ctx, pk);
  }

  const target*
  search_existing (context& ctx, const prerequisite_key& pk)
  {
    return pk.proj
      ? import_existing (ctx, pk)
      : search_existing_target (ctx, pk);
  }

  const target&
  search_new (context& ctx, const prerequisite_key& pk)
  {
    assert (ctx.phase == run_phase::load || ctx.phase == run_phase::match);

    if (const target* pt = search_existing_target (ctx, pk))
      return *pt;

    return create_new_target (ctx, pk);
  }

  pair<target&, ulock>
  search_new_locked (context& ctx, const prerequisite_key& pk)
  {
    assert (ctx.phase == run_phase::load || ctx.phase == run_phase::match);

    if (const target* pt = search_existing_target (ctx, pk))
      return {const_cast<target&> (*pt), ulock ()};

    return create_new_target_locked (ctx, pk);
  }

  const target&
  search (const target& t, name n, const scope& s, const target_type* tt)
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

    // @@ OUT: for now we assume the prerequisite's out is undetermined.
    //         Would need to pass a pair of names.
    //
    prerequisite_key pk {
      n.proj, {tt, &n.dir, q ? &empty_dir_path : &out, &n.value, ext}, &s};

    return q
      ? import_existing (s.ctx, pk)
      : search_existing_target (s.ctx, pk);
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
  target_lock
  lock_impl (action a, const target& ct, optional<scheduler::work_queue> wq)
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

    atomic_count& task_count (ct[a].task_count);

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
          return target_lock {a, nullptr, e - b};

        // We also unlock the phase for the duration of the wait. Why?
        // Consider this scenario: we are trying to match a dir{} target whose
        // buildfile still needs to be loaded. Let's say someone else started
        // the match before us. So we wait for their completion and they wait
        // to switch the phase to load. Which would result in a deadlock
        // unless we release the phase.
        //
        phase_unlock u (ct.ctx, true /* unlock */, true /* delay */);
        e = ctx.sched.wait (busy - 1, task_count, u, *wq);
      }

      // We don't lock already applied or executed targets.
      //
      if (e >= appl)
        return target_lock {a, nullptr, e - b};
    }

    // We now have the lock. Analyze the old value and decide what to do.
    //
    target& t (const_cast<target&> (ct));
    target::opstate& s (t[a]);

    size_t offset;
    if (e <= b)
    {
      // First lock for this operation.
      //
      s.rule = nullptr;
      s.dependents.store (0, memory_order_release);

      offset = target::offset_touched;
    }
    else
    {
      offset = e - b;
      assert (offset == target::offset_touched ||
              offset == target::offset_tried   ||
              offset == target::offset_matched);
    }

    return target_lock {a, &t, offset};
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
    ctx.sched.resume (task_count);
  }

  target&
  add_adhoc_member (target& t,
                    const target_type& tt,
                    dir_path dir,
                    dir_path out,
                    string n)
  {
    tracer trace ("add_adhoc_member");

    const_ptr<target>* mp (&t.adhoc_member);
    for (; *mp != nullptr && !(*mp)->is_a (tt); mp = &(*mp)->adhoc_member) ;

    if (*mp != nullptr) // Might already be there.
      return **mp;

    target* m (nullptr);
    {
      pair<target&, ulock> r (
        t.ctx.targets.insert_locked (tt,
                                     move (dir),
                                     move (out),
                                     move (n),
                                     nullopt /* ext     */,
                                     target_decl::implied,
                                     trace,
                                     true /* skip_find */));

      if (r.second) // Inserted.
      {
        m = &r.first;
        m->group = &t;
      }
    }

    assert (m != nullptr);
    *mp = m;

    return *m;
  };

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

  // Return the matching rule or NULL if no match and try_match is true.
  //
  const rule_match*
  match_rule (action a, target& t, const rule* skip, bool try_match)
  {
    const scope& bs (t.base_scope ());

    // Match rules in project environment.
    //
    auto_project_env penv;
    if (const scope* rs = bs.root_scope ())
      penv = auto_project_env (*rs);

    match_extra& me (t[a].match_extra);

    // First check for an ad hoc recipe.
    //
    // Note that a fallback recipe is preferred over a non-fallback rule.
    //
    if (!t.adhoc_recipes.empty ())
    {
      auto df = make_diag_frame (
        [a, &t](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while matching ad hoc recipe to " << diag_do (a, t);
        });

      auto match = [a, &t, &me] (const adhoc_rule& r, bool fallback) -> bool
      {
        me.init (fallback);

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

      auto b (t.adhoc_recipes.begin ()), e (t.adhoc_recipes.end ());
      auto i (find_if (
                b, e,
                [&match, ca] (const shared_ptr<adhoc_rule>& r)
                {
                  auto& as (r->actions);
                  return (find (as.begin (), as.end (), ca) != as.end () &&
                          match (*r, false));
                }));

      if (i == e)
      {
        // See if we have a fallback implementation.
        //
        // See the adhoc_rule::reverse_fallback() documentation for details on
        // what's going on here.
        //
        i = find_if (
          b, e,
          [&match, ca, &t] (const shared_ptr<adhoc_rule>& r)
          {
            auto& as (r->actions);

            // Note that the rule could be there but not match (see above),
            // thus this extra check.
            //
            return (find (as.begin (), as.end (), ca) == as.end () &&
                    r->reverse_fallback (ca, t.type ())            &&
                    match (*r, true));
          });
      }

      if (i != e)
        return &(*i)->rule_match;
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
              using fallback_rule = adhoc_rule_pattern::fallback_rule;

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
                if (auto* fr =
                      dynamic_cast<const fallback_rule*> (&r->second.get ()))
                {
                  if ((r = find_fallback (*fr)) == nullptr)
                    continue;
                }
              }

              const string& n (r->first);
              const rule& ru (r->second);

              if (&ru == skip)
                continue;

              me.init (oi == 0 /* fallback */);
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

              diag_record dr;
              for (++i; i != rs.second; ++i)
              {
                const rule_match* r1 (&*i);

                if (oi == 0)
                {
                  if (auto* fr =
                        dynamic_cast<const fallback_rule*> (&r1->second.get ()))
                  {
                    if ((r1 = find_fallback (*fr)) == nullptr)
                      continue;
                  }
                }

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
                  match_extra me1;
                  me1.init (oi == 0);
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
                return r;
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

    me.free ();
    return re;
  }

  // If step is true then perform only one step of the match/apply sequence.
  //
  // If try_match is true, then indicate whether there is a rule match with
  // the first half of the result.
  //
  static pair<bool, target_state>
  match_impl (target_lock& l,
              bool step = false,
              bool try_match = false)
  {
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
        assert (!step);

        const target& g (*t.group);

        // It feels natural to "convert" this call to the one for the group,
        // including the try_match part. Semantically, we want to achieve the
        // following:
        //
        // [try_]match (a, g);
        // match_recipe (l, group_recipe);
        //
        auto df = make_diag_frame (
          [a, &t](const diag_record& dr)
          {
            if (verb != 0)
              dr << info << "while matching group rule to " << diag_do (a, t);
          });

        pair<bool, target_state> r (match_impl (a, g, 0, nullptr, try_match));

        if (r.first)
        {
          if (r.second != target_state::failed)
          {
            match_inc_dependents (a, g);
            match_recipe (l, group_recipe);
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

          const rule_match* r (match_rule (a, t, nullptr, try_match));

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
          // Apply.
          //
          set_recipe (l, apply_impl (a, t, *s.rule));
          l.offset = target::offset_applied;
          break;
        }
      default:
        assert (false);
      }
    }
    catch (const failed&)
    {
      // As a sanity measure clear the target data since it can be incomplete
      // or invalid (mark()/unmark() should give you some ideas).
      //
      clear_target (a, t);

      s.state = target_state::failed;
      l.offset = target::offset_applied;
    }

    return make_pair (true, s.state);
  }

  // If try_match is true, then indicate whether there is a rule match with
  // the first half of the result.
  //
  pair<bool, target_state>
  match_impl (action a,
              const target& ct,
              size_t start_count,
              atomic_count* task_count,
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
                 : nullopt));

    if (l.target != nullptr)
    {
      assert (l.offset < target::offset_applied); // Shouldn't lock otherwise.

      if (try_match && l.offset == target::offset_tried)
        return make_pair (false, target_state::unknown);

      if (task_count == nullptr)
        return match_impl (l, false /* step */, try_match);

      // Pass "disassembled" lock since the scheduler queue doesn't support
      // task destruction.
      //
      target_lock::data ld (l.release ());

      // Also pass our diagnostics and lock stacks (this is safe since we
      // expect the caller to wait for completion before unwinding its stack).
      //
      if (ct.ctx.sched.async (
            start_count,
            *task_count,
            [a, try_match] (const diag_frame* ds,
                            const target_lock* ls,
                            target& t, size_t offset)
            {
              // Switch to caller's diag and lock stacks.
              //
              diag_frame::stack_guard dsg (ds);
              target_lock::stack_guard lsg (ls);

              try
              {
                phase_lock pl (t.ctx, run_phase::match); // Throws.
                {
                  target_lock l {a, &t, offset}; // Reassemble.
                  match_impl (l, false /* step */, try_match);
                  // Unlock within the match phase.
                }
              }
              catch (const failed&) {} // Phase lock failure.
            },
            diag_frame::stack (),
            target_lock::stack (),
            ref (*ld.target),
            ld.offset))
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

  static group_view
  resolve_members_impl (action a, const target& g, target_lock&& l)
  {
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
        if (match_impl (l, true).second == target_state::failed)
          throw failed ();

        if ((r = g.group_members (a)).members != nullptr)
          break;

        // To apply ...
      }
      // Fall through.
    case target::offset_matched:
      {
        // @@ Doing match without execute messes up our target_count. Does
        //    not seem like it will be easy to fix (we don't know whether
        //    someone else will execute this target).
        //
        //    What if we always do match & execute together? After all,
        //    if a group can be resolved in apply(), then it can be
        //    resolved in match()! Feels a bit drastic.
        //
        //    But, this won't be a problem if the target returns noop_recipe.
        //    And perhaps it's correct to fail if it's not noop_recipe but
        //    nobody executed it? Maybe not.
        //
        //    Another option would be to have a count for such "matched but
        //    may not be executed" targets and then make sure target_count
        //    is less than that at the end. Though this definitelt makes it
        //    less exact (since we can end up executed this target but not
        //    some other). Maybe we can increment and decrement such targets
        //    in a separate count (i.e., mark their recipe as special or some
        //    such).
        //

        // Apply (locked).
        //
        if (match_impl (l, true).second == target_state::failed)
          throw failed ();

        if ((r = g.group_members (a)).members != nullptr)
          break;

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

  void
  resolve_group_impl (action, const target&, target_lock l)
  {
    match_impl (l, true /* step */, true /* try_match */);
  }

  template <typename R, typename S>
  static void
  match_prerequisite_range (action a, target& t,
                            R&& r,
                            const S& ms,
                            const scope* s)
  {
    auto& pts (t.prerequisite_targets[a]);

    // Start asynchronous matching of prerequisites. Wait with unlocked phase
    // to allow phase switching.
    //
    wait_guard wg (t.ctx, t.ctx.count_busy (), t[a].task_count, true);

    size_t i (pts.size ()); // Index of the first to be added.
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

      if (pt.target == nullptr || (s != nullptr && !pt.target->in (*s)))
        continue;

      match_async (a, *pt.target, t.ctx.count_busy (), t[a].task_count);
      pts.push_back (move (pt));
    }

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
                       const scope* s)
  {
    match_prerequisite_range (a, t, group_prerequisites (t), ms, s);
  }

  void
  match_prerequisite_members (action a, target& t,
                              const match_search_member& msm,
                              const scope* s)
  {
    match_prerequisite_range (a, t, group_prerequisite_members (a, t), msm, s);
  }

  void
  match_members (action a, target& t, const target* const* ts, size_t n)
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
                 target& t,
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
  inject_fsdir (action a, target& t, bool prereq, bool parent)
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

    if (r != nullptr)
    {
      // Make it ad hoc so that it doesn't end up in prerequisite_targets
      // after execution.
      //
      match_sync (a, *r);
      t.prerequisite_targets[a].emplace_back (r, include_type::adhoc);
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
        ts = t[a].state = target_state::unchanged;
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

  void
  update_backlink (const file& f, const path& l, bool changed, backlink_mode m)
  {
    using mode = backlink_mode;

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
    if (verb <= 2)
    {
      if (changed || !butl::entry_exists (l,
                                          false /* follow_symlinks */,
                                          true  /* ignore_errors */))
      {
        const char* c (nullptr);
        switch (m)
        {
        case mode::link:
        case mode::symbolic:  c = verb >= 2 ? "ln -s" : "ln";         break;
        case mode::hard:      c = "ln";                               break;
        case mode::copy:
        case mode::overwrite: c = l.to_directory () ? "cp -r" : "cp"; break;
        }

        // Note: 'ln foo/ bar/' means a different thing.
        //
        if (verb >= 2)
          text << c << ' ' << p.string () << ' ' << l.string ();
        else
          text << c << ' ' << f << " -> " << d;
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

    using mode = backlink_mode;

    dir_path d (l.directory ());

    if (verb <= 2)
    {
      if (changed || !butl::entry_exists (l,
                                          false /* follow_symlinks */,
                                          true  /* ignore_errors */))
      {
        const char* c (nullptr);
        switch (m)
        {
        case mode::link:
        case mode::symbolic:  c = verb >= 2 ? "ln -s" : "ln";         break;
        case mode::hard:      c = "ln";                               break;
        case mode::copy:
        case mode::overwrite: c = l.to_directory () ? "cp -r" : "cp"; break;
        }

        if (verb >= 2)
          text << c << ' ' << p.string () << ' ' << l.string ();
        else
          text << c << ' ' << p.string () << " -> " << d;
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
    using mode = backlink_mode;

    bool d (l.to_directory ());
    mode m (om); // Keep original mode.

    auto print = [&p, &l, &m, verbosity, d] ()
    {
      if (verb >= verbosity)
      {
        const char* c (nullptr);
        switch (m)
        {
        case mode::link:
        case mode::symbolic:  c = "ln -sf";           break;
        case mode::hard:      c = "ln -f";            break;
        case mode::copy:
        case mode::overwrite: c = d ? "cp -r" : "cp"; break;
        }

        text << c << ' ' << p.string () << ' ' << l.string ();
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

              for (const auto& de:
                     dir_iterator (fr, false /* ignore_dangling */))
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

    reference_wrapper<const path_type> target;
    backlink_mode mode;

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
  // there will be just one backlink so optimize for that.
  //
  using backlinks = small_vector<backlink, 1>;

  static optional<backlink_mode>
  backlink_test (const target& t, const lookup& l)
  {
    using mode = backlink_mode;

    optional<mode> r;
    const string& v (cast<string> (l));

    if      (v == "true")      r = mode::link;
    else if (v == "symbolic")  r = mode::symbolic;
    else if (v == "hard")      r = mode::hard;
    else if (v == "copy")      r = mode::copy;
    else if (v == "overwrite") r = mode::overwrite;
    else if (v != "false")
      fail << "invalid backlink variable value '" << v << "' "
           << "specified for target " << t;

    return r;
  }

  static optional<backlink_mode>
  backlink_test (action a, target& t)
  {
    context& ctx (t.ctx);

    // Note: the order of these checks is from the least to most expensive.

    // Only for plain update/clean.
    //
    if (a.outer () || (a != perform_update_id && a != perform_clean_id))
      return nullopt;

    // Only file-based targets in the out tree can be backlinked.
    //
    if (!t.out.empty () || !t.is_a<file> ())
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

    return l ? backlink_test (t, l) : nullopt;
  }

  static backlinks
  backlink_collect (action a, target& t, backlink_mode m)
  {
    using mode = backlink_mode;

    context& ctx (t.ctx);
    const scope& s (t.base_scope ());

    backlinks bls;
    auto add = [&bls, &s] (const path& p, mode m)
    {
      bls.emplace_back (p,
                        s.src_path () / p.leaf (s.out_path ()),
                        m,
                        !s.ctx.dry_run /* active */);
    };

    // First the target itself.
    //
    add (t.as<file> ().path (), m);

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
        // Check for a custom backlink mode for this member. If none, then
        // inherit the one from the group (so if the user asked to copy .exe,
        // we will also copy .pdb).
        //
        // Note that we want to avoid group or tt/patter-spec lookup. And
        // since this is an ad hoc member (which means it was either declared
        // in the buildfile or added by the rule), we assume that the value,
        // if any, will be set as a target or rule-specific variable.
        //
        lookup l (mt->state[a].vars[ctx.var_backlink]);

        if (!l)
          l = mt->vars[ctx.var_backlink];

        optional<mode> bm (l ? backlink_test (*mt, l) : m);

        if (bm)
          add (*p, *bm);
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
  backlink_update_post (target& t, target_state ts, backlinks& bls)
  {
    if (ts == target_state::failed)
      return; // Let auto rm clean things up.

    // Make backlinks.
    //
    for (auto b (bls.begin ()), i (b); i != bls.end (); ++i)
    {
      const backlink& bl (*i);

      if (i == b)
        update_backlink (t.as<file> (),
                         bl.path,
                         ts == target_state::changed,
                         bl.mode);
      else
        update_backlink (t.ctx, bl.target, bl.path, bl.mode);
    }

    // Cancel removal.
    //
    if (!t.ctx.dry_run)
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
      backlinks bls;
      optional<backlink_mode> blm (backlink_test (a, t));

      if (blm)
      {
        if (a == perform_update_id)
          bls = backlink_update_pre (a, t, *blm);
        else
          backlink_clean_pre (a, t, *blm);
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
          backlink_update_post (t, ts, bls);
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
    //
    if (!s.recipe_keep)
      s.recipe = nullptr;

    // Decrement the target count (see set_recipe() for details).
    //
    // Note that here we cannot rely on s.state being group because of the
    // postponment logic (see excute_recipe() for details).
    //
    if (a.inner () && !s.recipe_group_action)
      ctx.target_count.fetch_sub (1, memory_order_relaxed);

    // Decrement the task count (to count_executed) and wake up any threads
    // that might be waiting for this target.
    //
    size_t tc (s.task_count.fetch_sub (
                 target::offset_busy - target::offset_executed,
                 memory_order_release));
    assert (tc == ctx.count_busy ());
    ctx.sched.resume (s.task_count);

    return ts;
  }

  target_state
  execute_impl (action a,
                const target& ct,
                size_t start_count,
                atomic_count* task_count)
  {
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
        if (t.is_a<dir> ())
          execute_recipe (a, t, nullptr /* recipe */);

        s.task_count.store (exec, memory_order_release);
        ctx.sched.resume (s.task_count);
      }
      else
      {
        if (task_count == nullptr)
          return execute_impl (a, t);

        // Pass our diagnostics stack (this is safe since we expect the
        // caller to wait for completion before unwinding its diag stack).
        //
        if (ctx.sched.async (start_count,
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
      // Either busy or already executed.
      //
      if (tc >= busy) return target_state::busy;
      else            assert (tc == exec);
    }

    return t.executed_state (a, false);
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

    if (s.task_count.compare_exchange_strong (
          tc,
          busy,
          memory_order_acq_rel,  // Synchronize on success.
          memory_order_acquire)) // Synchronize on failure.
    {
      if (s.state == target_state::unknown)
      {
        if (task_count == nullptr)
          return execute_impl (a, t);

        if (ctx.sched.async (start_count,
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
      else
      {
        assert (s.state == target_state::unchanged ||
                s.state == target_state::failed);

        if (s.state == target_state::unchanged)
        {
          if (t.is_a<dir> ())
            execute_recipe (a, t, nullptr /* recipe */);
        }

        s.task_count.store (exec, memory_order_release);
        ctx.sched.resume (s.task_count);
      }
    }
    else
    {
      // Either busy or already executed.
      //
      if (tc >= busy) return target_state::busy;
      else            assert (tc == exec);
    }

    return t.executed_state (a, false);
  }

  bool
  update_during_match (tracer& trace, action a, const target& t, timestamp ts)
  {
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
    prerequisite_targets& pts (t.prerequisite_targets[a]);

    // On the first pass detect and handle unchanged tragets. Note that we
    // have to do it in a separate pass since we cannot call matched_state()
    // once we've switched the phase.
    //
    size_t n (0);

    for (prerequisite_target& p: pts)
    {
      if ((p.include & mask) != 0)
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
      if ((p.include & mask) != 0 && p.data != 0)
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
      if ((p.include & mask) != 0 && p.data != 0)
      {
        execute_direct_async (a, *p.target, busy, tc);
      }
    }

    wg.wait ();

    // Finish execution and process the result.
    //
    for (prerequisite_target& p: pts)
    {
      if ((p.include & mask) != 0 && p.data != 0)
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
    text << "noop action triggered for " << diag_doing (a, t);
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
      ctx.sched.wait (ctx.count_executed (),
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
              text << dp << " is current working directory, not removing";
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
                       const clean_adhoc_extras& adhoc_extras)
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

        if (r == target_state::changed && ep.empty ())
          ep = *mp;

        er |= r;
      }
    }

    // Now clean the primary target and its prerequisited in the reverse order
    // of update: first remove the file, then clean the prerequisites.
    //
    if (clean && !fp.empty () && rmfile (fp, ft))
      tr = target_state::changed;

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
        if (ed)
          text << "rm -r " << path_cast<dir_path> (ep);
        else
          text << "rm " << ep;
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
          if (rmfile (m->as<file> ().path (), *m))
            tr |= target_state::changed;
        }
      }
    }

    g.mtime (timestamp_nonexistent);

    if (tr != target_state::changed && er == target_state::changed)
    {
      if (verb > (ctx.current_diag_noise ? 0 : 1) && verb < 3)
      {
        if (ed)
          text << "rm -r " << path_cast<dir_path> (ep);
        else
          text << "rm " << ep;
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
