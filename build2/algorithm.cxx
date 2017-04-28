// file      : build2/algorithm.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/algorithm>

#include <build2/scope>
#include <build2/target>
#include <build2/rule>
#include <build2/file> // import()
#include <build2/search>
#include <build2/context>
#include <build2/filesystem>
#include <build2/diagnostics>
#include <build2/prerequisite>

using namespace std;
using namespace butl;

namespace build2
{
  const target&
  search (const target& t, const prerequisite_key& pk)
  {
    assert (phase == run_phase::match);

    // If this is a project-qualified prerequisite, then this is import's
    // business.
    //
    if (pk.proj)
      return import (pk);

    if (const target* pt = pk.tk.type->search (t, pk))
      return *pt;

    return create_new_target (pk);
  }

  const target&
  search (const target& t, name n, const scope& s)
  {
    assert (phase == run_phase::match);

    optional<string> ext;
    const target_type* tt (s.find_target_type (n, ext));

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
    assert (phase == run_phase::match || phase == run_phase::execute);

    name n (cn);
    optional<string> ext;
    const target_type* tt (s.find_target_type (n, ext));

    // For now we treat an unknown target type as an unknown target. Seems
    // logical.
    //
    if (tt == nullptr)
      return nullptr;

    if (!n.dir.empty ())
      n.dir.normalize (false, true); // Current dir collapses to an empty one.

    bool q (cn.qualified ());

    // @@ OUT: for now we assume the prerequisite's out is undetermined.
    //         Would need to pass a pair of names.
    //
    prerequisite_key pk {
      n.proj, {tt, &n.dir, q ? &empty_dir_path : &out, &n.value, ext}, &s};

    return q ? import_existing (pk) : search_existing_target (pk);
  }

  // If the work_queue is not present, then we don't wait.
  //
  target_lock
  lock_impl (action a, const target& ct, optional<scheduler::work_queue> wq)
  {
    assert (phase == run_phase::match);

    // Most likely the target's state is (count_touched - 1), that is, 0 or
    // previously executed, so let's start with that.
    //
    size_t b (target::count_base ());
    size_t e (b + target::offset_touched - 1);

    size_t lock (b + target::offset_locked);
    size_t busy (b + target::offset_busy);

    for (;;)
    {
      // First try to grab the spin lock which we later may upgrade to busy.
      //
      if (ct.task_count.compare_exchange_strong (
            e,
            lock,
            memory_order_acq_rel,  // Synchronize on success.
            memory_order_acquire)) // Synchronize on failure.
      {
        break;
      }

      while (e == lock || e >= busy)
      {
        // Wait for the count to drop below busy if someone is already working
        // on this target.
        //
        // We also unlock the phase for the duration of the wait. Why? Consider
        // this scenario: we are trying to match a dir{} target whose buildfile
        // still needs to be loaded. Let's say someone else started the match
        // before us. So we wait for their completion and they wait to switch
        // the phase to load. Which would result in a deadlock unless we release
        // the phase.
        //
        if (e >= busy)
        {
          if (!wq)
            return target_lock {nullptr, e - b};

          phase_unlock ul;
          e = sched.wait (busy - 1, ct.task_count, *wq);
        }

        // Spin if it is locked.
        //
        for (; e == lock; e = ct.task_count.load (memory_order_acquire))
          this_thread::yield ();
      }
    }

    // We now have the sping lock. Analyze the old value and decide what to
    // do.
    //
    target& t (const_cast<target&> (ct));

    size_t offset;
    if (e <= b)
    {
      // First lock for this operation.
      //
      t.action = a;
      t.dependents.store (0, memory_order_release);
      offset = target::offset_touched;
    }
    else
    {
      offset = e - b;

      switch (offset)
      {
      case target::offset_executed:
        {
          if (t.action == a || t.action > a)
          {
            // We don't lock already executed targets.
            //
            t.task_count.store (e, memory_order_release);
            return target_lock {nullptr, target::offset_executed};
          }

          // Override, fall through.
          //
          assert (a > t.action);
        }
      case target::offset_touched:
      case target::offset_matched:
      case target::offset_applied:
        {
          if (a > t.action)
          {
            // Only noop_recipe can be overridden.
            //
            if (offset >= target::offset_applied)
            {
              recipe_function** f (t.recipe_.target<recipe_function*> ());
              assert (f != nullptr && *f == &noop_action);
            }

            t.action = a;
            offset = target::offset_touched; // Back to just touched.
          }
          else
          {
            assert (t.action > a || a == t.action);

            // Release the lock if already applied for this action. This is
            // necessary no to confuse execute since otherwise it might see
            // that the target is busy and assume that someone is already
            // executing it. Note that we cannot handle this case in the loop
            // above since we need to lock target::action.
            //
            if (offset == target::offset_applied || t.action > a)
            {
              // Release the spin lock.
              //
              t.task_count.store (e, memory_order_release);
              return target_lock {nullptr, offset};
            }
          }

          break;
        }
      default:
        assert (false);
      }
    }

    // We are keeping it so upgrade to busy.
    //
    t.task_count.store (busy, memory_order_release);
    return target_lock (&t, offset);
  }

  void
  unlock_impl (target& t, size_t offset)
  {
    assert (phase == run_phase::match);

    // Set the task count and wake up any threads that might be waiting for
    // this target.
    //
    t.task_count.store (offset + target::count_base (), memory_order_release);
    sched.resume (t.task_count);
  }

  // Return the matching rule and the recipe action.
  //
  pair<const pair<const string, reference_wrapper<const rule>>*, action>
  match_impl (action a, target& t, const rule* skip, bool f)
  {
    // Clear the resolved targets list before calling match(). The rule is
    // free to modify this list in match() (provided that it matches) in order
    // to, for example, prepare it for apply().
    //
    t.clear_data ();
    t.prerequisite_targets.clear ();

    // If this is a nested operation, first try the outer operation.
    // This allows a rule to implement a "precise match", that is,
    // both inner and outer operations match.
    //
    for (operation_id oo (a.outer_operation ()), io (a.operation ()),
           o (oo != 0 ? oo : io);
         o != 0;
         o = (oo != 0 && o != io ? io : 0))
    {
      // Adjust action for recipe: on the first iteration we want it
      // {inner, outer} (which is the same as 'a') while on the second
      // -- {inner, 0}. Note that {inner, 0} is the same or "stronger"
      // (i.e., overrides; see action::operator<()) than 'a'. This
      // allows "unconditional inner" to override "inner for outer"
      // recipes.
      //
      action ra (a.meta_operation (), io, o != oo ? 0 : oo);

      const scope& bs (t.base_scope ());

      for (auto tt (&t.type ()); tt != nullptr; tt = tt->base)
      {
        // Search scopes outwards, stopping at the project root.
        //
        for (const scope* s (&bs);
             s != nullptr;
             s = s->root () ? global_scope : s->parent_scope ())
        {
          const operation_rule_map* om (s->rules[a.meta_operation ()]);

          if (om == nullptr)
            continue; // No entry for this meta-operation id.

          // First try the map for the actual operation. If that doesn't yeld
          // anything, try the wildcard map.
          //
          for (size_t oi (o), oip (o); oip != 0; oip = oi, oi = 0)
          {
            const target_type_rule_map* ttm ((*om)[oi]);

            if (ttm == nullptr)
              continue; // No entry for this operation id.

            if (ttm->empty ())
              continue; // Empty map for this operation id.

            auto i (ttm->find (tt));

            if (i == ttm->end () || i->second.empty ())
              continue; // No rules registered for this target type.

            const auto& rules (i->second); // Hint map.

            // @@ TODO
            //
            // Different rules can be used for different operations (update
            // vs test is a good example). So, at some point, we will probably
            // have to support a list of hints or even an operation-hint map
            // (e.g., 'hint=cxx test=foo' if cxx supports the test operation
            // but we want the foo rule instead). This is also the place where
            // the '{build clean}=cxx' construct (which we currently do not
            // support) can come handy.
            //
            // Also, ignore the hint (that is most likely ment for a different
            // operation) if this is a unique match.
            //
            string hint;
            auto rs (rules.size () == 1
                     ? make_pair (rules.begin (), rules.end ())
                     : rules.find_prefix (hint));

            for (auto i (rs.first); i != rs.second; ++i)
            {
              const auto& r (*i);
              const string& n (r.first);
              const rule& ru (r.second);

              if (&ru == skip)
                continue;

              match_result m (false);
              {
                auto df = make_diag_frame (
                  [ra, &t, &n](const diag_record& dr)
                  {
                    if (verb != 0)
                      dr << info << "while matching rule " << n << " to "
                         << diag_do (ra, t);
                  });

                if (!(m = ru.match (ra, t, hint)))
                  continue;

                if (m.recipe_action.valid ())
                  assert (m.recipe_action > ra);
                else
                  m.recipe_action = ra; // Default, if not set.
              }

              // Do the ambiguity test.
              //
              bool ambig (false);

              diag_record dr;
              for (++i; i != rs.second; ++i)
              {
                const string& n1 (i->first);
                const rule& ru1 (i->second);

                {
                  auto df = make_diag_frame (
                    [ra, &t, &n1](const diag_record& dr)
                    {
                      if (verb != 0)
                        dr << info << "while matching rule " << n1 << " to "
                           << diag_do (ra, t);
                    });

                  // @@ TODO: this makes target state in match() undetermined
                  //    so need to fortify rules that modify anything in match
                  //    to clear things.
                  //
                  if (!ru1.match (ra, t, hint))
                    continue;
                }

                if (!ambig)
                {
                  dr << fail << "multiple rules matching "
                     << diag_doing (ra, t)
                     << info << "rule " << n << " matches";
                  ambig = true;
                }

                dr << info << "rule " << n1 << " also matches";
              }

              if (!ambig)
                return  make_pair (&r, m.recipe_action);
              else
                dr << info << "use rule hint to disambiguate this match";
            }
          }
        }
      }
    }

    if (f)
    {
      diag_record dr;
      dr << fail << "no rule to " << diag_do (a, t);

      if (verb < 4)
        dr << info << "re-run with --verbose 4 for more information";
    }

    return pair<const pair<const string, reference_wrapper<const rule>>*,
                action> {nullptr, a};
  }

  recipe
  apply_impl (target& t,
              const pair<const string, reference_wrapper<const rule>>& r,
              action a)
  {
    auto df = make_diag_frame (
      [a, &t, &r](const diag_record& dr)
      {
        if (verb != 0)
          dr << info << "while applying rule " << r.first << " to "
             << diag_do (a, t);
      });

    // @@ We could also allow the rule to change the recipe action in
    // apply(). Could be useful with delegates.
    //
    return r.second.get ().apply (a, t);
  }

  // If step is true then perform only one step of the match/apply sequence.
  //
  static target_state
  match_impl (action a, target_lock& l, bool step = false)
  {
    assert (l.target != nullptr);
    target& t (*l.target);

    try
    {
      // Continue from where the target has been left off.
      //
      switch (l.offset)
      {
      case target::offset_touched:
        {
          // Match.
          //
          auto mr (match_impl (a, t, nullptr));
          t.rule = mr.first;
          t.action = mr.second; // In case overriden.
          l.offset = target::offset_matched;

          if (step)
            return target_state::unknown; // t.state_ not set yet.

          // Fall through.
        }
      case target::offset_matched:
        {
          // Apply.
          //
          t.recipe (apply_impl (t, *t.rule, t.action));
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
      // or invalid (mark()/unmark() should give you some for ideas).
      //
      t.clear_data ();
      t.prerequisite_targets.clear ();

      t.state_ = target_state::failed;
      l.offset = target::offset_applied;
    }

    return t.state_;
  }

  target_state
  match (action a,
         const target& ct,
         size_t start_count,
         atomic_count* task_count)
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

    if (l.target == nullptr)
    {
      // Already matched, executed, or busy.
      //
      if (l.offset >= target::offset_busy)
        return target_state::busy;

      // Fall through.
    }
    else if (l.offset != target::offset_applied) // Nothing to do if applied.
    {
      if (task_count == nullptr)
        return match_impl (a, l);

      // Pass "disassembled" lock since the scheduler queue doesn't support
      // task destruction. Also pass our diagnostics stack (this is safe since
      // we expect the caller to wait for completion before unwinding its diag
      // stack).
      //
      if (sched.async (start_count,
                       *task_count,
                       [a] (target& t,
                            size_t offset,
                            const diag_frame* ds)
                       {
                         diag_frame df (ds);
                         phase_lock pl (run_phase::match);
                         {
                           target_lock l {&t, offset}; // Reassemble.
                           match_impl (a, l);
                           // Unlock withing the match phase.
                         }
                       },
                       ref (*l.release ()),
                       l.offset,
                       diag_frame::stack))
        return target_state::postponed; // Queued.

      // Matched synchronously, fall through.
    }

    return ct.matched_state (a, false);
  }

  group_view
  resolve_group_members_impl (action a, const target& g, target_lock l)
  {
    // Note that we will be unlocked if the target is already applied.
    //
    group_view r;

    // Continue from where the target has been left off.
    //
    switch (l.offset)
    {
    case target::offset_touched:
      {
        // Match (locked).
        //
        if (match_impl (a, l, true) == target_state::failed)
          throw failed ();

        if ((r = g.group_members (a)).members != nullptr)
          break;

        // Fall through to apply.
      }
    case target::offset_matched:
      {
        // Apply (locked).
        //
        if (match_impl (a, l, true) == target_state::failed)
          throw failed ();

        if ((r = g.group_members (a)).members != nullptr)
          break;

        // Unlock and fall through to execute.
        //
        l.unlock ();
      }
    case target::offset_applied:
      {
        // Execute (unlocked).
        //
        // Note that we use execute_direct() rather than execute() here to
        // sidestep the dependents count logic. In this context, this is by
        // definition the first attempt to execute this rule (otherwise we
        // would have already known the members list) and we really do need
        // to execute it now.
        //
        {
          phase_switch ps (run_phase::execute);
          execute_direct (a, g);
        }

        r = g.group_members (a);
        break;
      }
    }

    return r;
  }

  template <typename R>
  static void
  match_prerequisite_range (action a, target& t, R&& r, const scope* s)
  {
    auto& pts (t.prerequisite_targets);

    // Start asynchronous matching of prerequisites. Wait with unlocked phase
    // to allow phase switching.
    //
    wait_guard wg (target::count_busy (), t.task_count, true);

    size_t i (pts.size ()); // Index of the first to be added.
    for (auto&& p: forward<R> (r))
    {
      const target& pt (search (t, p));

      if (s != nullptr && !pt.in (*s))
        continue;

      match_async (a, pt, target::count_busy (), t.task_count);
      pts.push_back (&pt);
    }

    wg.wait ();

    // Finish matching all the targets that we have started.
    //
    for (size_t n (pts.size ()); i != n; ++i)
    {
      const target& pt (*pts[i]);
      match (a, pt);
    }
  }

  void
  match_prerequisites (action a, target& t, const scope* s)
  {
    match_prerequisite_range (a, t, group_prerequisites (t), s);
  }

  void
  match_prerequisite_members (action a, target& t, const scope* s)
  {
    match_prerequisite_range (a, t, group_prerequisite_members (a, t), s);
  }

  void
  match_members (action a, target& t, const target* ts[], size_t n)
  {
    // Pretty much identical to match_prerequisite_range() except we don't
    // search.
    //
    wait_guard wg (target::count_busy (), t.task_count, true);

    for (size_t i (0); i != n; ++i)
    {
      const target* m (ts[i]);

      if (m == nullptr)
        continue;

      match_async (a, *m, target::count_busy (), t.task_count);
    }

    wg.wait ();

    // Finish matching all the targets that we have started.
    //
    for (size_t i (0); i != n; ++i)
    {
      const target* m (ts[i]);

      if (m == nullptr)
        continue;

      match (a, *m);
    }
  }

  const fsdir*
  inject_fsdir (action a, target& t, bool parent)
  {
    tracer trace ("inject_fsdir");

    // If t is a directory (name is empty), say foo/bar/, then t is bar and
    // its parent directory is foo/.
    //
    const dir_path& d (parent && t.name.empty () ? t.dir.directory () : t.dir);

    const scope& bs (scopes.find (d));
    const scope* rs (bs.root_scope ());

    // If root scope is NULL, then this can mean that we are out of any
    // project or if the directory is in src_root. In both cases we don't
    // inject anything.
    //
    // Note that we also used to bail out if this is the root of the
    // project. But that proved not to be such a great idea in case of
    // subprojects (e.g., tests/).
    //
    if (rs == nullptr)
      return nullptr;

    // Handle the src_root = out_root.
    //
    if (d.sub (rs->src_path ()))
      return nullptr;

    l6 ([&]{trace << d << " for " << t;});

    // Target is in the out tree, so out directory is empty.
    //
    const fsdir* r (
      &search<fsdir> (t, d, dir_path (), string (), nullptr, nullptr));
    match (a, *r);
    t.prerequisite_targets.emplace_back (r);
    return r;
  }

  static target_state
  execute_impl (action a, target& t)
  {
    assert (t.task_count.load (memory_order_consume) == target::count_busy ()
            && t.state_ == target_state::unknown);

    target_state ts;

    try
    {
      auto df = make_diag_frame (
        [a, &t](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while " << diag_doing (a, t);
        });

      ts = t.recipe_ (a, t);

      // See the recipe documentation for details on what's going on here.
      // Note that if the result is group, then the group's state can be
      // failed.
      //
      switch (t.state_ = ts)
      {
      case target_state::changed:
      case target_state::unchanged:
        break;
      case target_state::postponed:
        ts = t.state_ = target_state::unchanged;
        break;
      case target_state::group:
        ts = t.group->state_;
        break;
      default:
        assert (false);
      }
    }
    catch (const failed&)
    {
      ts = t.state_ = target_state::failed;
    }

    // Decrement the task count (to count_executed) and wake up any threads
    // that might be waiting for this target.
    //
    size_t tc (t.task_count.fetch_sub (
                 target::offset_busy - target::offset_executed,
                 memory_order_release));
    assert (tc == target::count_busy ());
    sched.resume (t.task_count);

    return ts;
  }

  target_state
  execute (action a,
           const target& ct,
           size_t start_count,
           atomic_count* task_count)
  {
    target& t (const_cast<target&> (ct)); // MT-aware.

    // Update dependency counts and make sure they are not skew.
    //
    size_t td (t.dependents.fetch_sub (1, memory_order_release));
    size_t gd (dependency_count.fetch_sub (1, memory_order_release));
    assert (td != 0 && gd != 0);
    td--;

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
    // Note that below we are going to treat the group state to postponed.
    // This is not a mistake: until we execute the recipe, we want to keep
    // returning postponed. And once the recipe is executed, it will reset the
    // state to group (see group_action()). To put it another way, the
    // execution of this member is postponed, not of the group.
    //
    // Note also that the target execution is postponed with regards to this
    // thread. For other threads the state will still be unknown (until they
    // try to execute it).
    //
    if (current_mode == execution_mode::last && td != 0)
      return target_state::postponed;

    // Try to atomically change applied to busy. Note that we are in the
    // execution phase so the target shall not be spin-locked.
    //
    size_t touc (target::count_touched ());
    size_t matc (target::count_matched ());
    size_t exec (target::count_executed ());
    size_t busy (target::count_busy ());

    for (size_t tc (target::count_applied ());;)
    {
      if (t.task_count.compare_exchange_strong (
            tc,
            busy,
            memory_order_acq_rel,  // Synchronize on success.
            memory_order_acquire)) // Synchronize on failure.
      {
        // Overriden touch/match-only or noop recipe.
        //
        if (tc == touc || tc == matc || t.state_ == target_state::unchanged)
        {
          t.state_ = target_state::unchanged;
          t.task_count.store (exec, memory_order_release);
          sched.resume (t.task_count);
        }
        else
        {
          if (task_count == nullptr)
            return execute_impl (a, t);

          // Pass our diagnostics stack (this is safe since we expect the
          // caller to wait for completion before unwinding its diag stack).
          //
          if (sched.async (start_count,
                           *task_count,
                           [a] (target& t, const diag_frame* ds)
                           {
                             diag_frame df (ds);
                             execute_impl (a, t);
                           },
                           ref (t),
                           diag_frame::stack))
            return target_state::unknown; // Queued.

          // Executed synchronously, fall through.
        }
      }
      else
      {
        // Normally either busy or already executed.
        //
        if (tc >= busy) return target_state::busy;
        else if (tc != exec)
        {
          // This can happen if we touched/matched (a noop) recipe which then
          // got overridden as part of group resolution but not all the way to
          // applied. In this case we treat it as noop.
          //
          assert ((tc == touc || tc == matc) && t.action > a);
          continue;
        }
      }

      break;
    }

    return t.executed_state (false);
  }

  target_state
  execute_direct (action a, const target& ct)
  {
    target& t (const_cast<target&> (ct)); // MT-aware.

    // Similar logic to match() above.
    //
    size_t touc (target::count_touched ());
    size_t matc (target::count_matched ());
    size_t exec (target::count_executed ());
    size_t busy (target::count_busy ());

    for (size_t tc (target::count_applied ());;)
    {
      if (t.task_count.compare_exchange_strong (
            tc,
            busy,
            memory_order_acq_rel,  // Synchronize on success.
            memory_order_acquire)) // Synchronize on failure.
      {
        if (tc == touc || tc == matc || t.state_ == target_state::unchanged)
        {
          t.state_ = target_state::unchanged;
          t.task_count.store (exec, memory_order_release);
          sched.resume (t.task_count);
        }
        else
          execute_impl (a, t);
      }
      else
      {
        // If the target is busy, wait for it.
        //
        if (tc >= busy) sched.wait (exec, t.task_count, scheduler::work_none);
        else if (tc != exec)
        {
          assert ((tc == touc || tc == matc) && t.action > a);
          continue;
        }
      }

      break;
    }

    return t.executed_state ();
  }

  // We use the target pointer mark mechanism to indicate whether the target
  // was already busy. Probably not worth it (we are saving an atomic load),
  // but what the hell. Note that this means we have to always "harvest" all
  // the targets to clear the mark.
  //
  target_state
  straight_execute_members (action a,
                            const target& t,
                            const target* ts[], size_t n)
  {
    target_state r (target_state::unchanged);

    // Start asynchronous execution of prerequisites.
    //
    wait_guard wg (target::count_busy (), t.task_count);

    for (size_t i (0); i != n; ++i)
    {
      const target*& mt (ts[i]);

      if (mt == nullptr) // Skipped.
        continue;

      target_state s (
        execute_async (
          a, *mt, target::count_busy (), t.task_count));

      if (s == target_state::postponed)
      {
        r |= s;
        mt = nullptr;
      }
      else if (s == target_state::busy)
        mark (mt);
    }

    wg.wait ();

    // Now all the targets in prerequisite_targets must be executed and
    // synchronized (and we have blanked out all the postponed ones).
    //
    for (size_t i (0); i != n; ++i)
    {
      const target*& mt (ts[i]);

      if (mt == nullptr)
        continue;

      // If the target was already busy, wait for its completion.
      //
      if (unmark (mt))
        sched.wait (
          target::count_executed (), mt->task_count, scheduler::work_none);

      r |= mt->executed_state ();
    }

    return r;
  }

  target_state
  reverse_execute_members (action a,
                           const target& t,
                           const target* ts[], size_t n)
  {
    // Pretty much as straight_execute_members() but in reverse order.
    //
    target_state r (target_state::unchanged);

    wait_guard wg (target::count_busy (), t.task_count);

    for (size_t i (n); i != 0; --i)
    {
      const target*& mt (ts[i - 1]);

      if (mt == nullptr)
        continue;

      target_state s (
        execute_async (
          a, *mt, target::count_busy (), t.task_count));

      if (s == target_state::postponed)
      {
        r |= s;
        mt = nullptr;
      }
      else if (s == target_state::busy)
        mark (mt);
    }

    wg.wait ();

    for (size_t i (n); i != 0; --i)
    {
      const target*& mt (ts[i - 1]);

      if (mt == nullptr)
        continue;

      if (unmark (mt))
        sched.wait (
          target::count_executed (), mt->task_count, scheduler::work_none);

      r |= mt->executed_state ();
    }

    return r;
  }

  pair<optional<target_state>, const target*>
  execute_prerequisites (const target_type* tt,
                         action a, const target& t,
                         const timestamp& mt, const prerequisite_filter& pf)
  {
    assert (current_mode == execution_mode::first);

    auto& pts (const_cast<target&> (t).prerequisite_targets); // MT-aware.

    // Pretty much as straight_execute_members() but hairier.
    //
    target_state rs (target_state::unchanged);

    wait_guard wg (target::count_busy (), t.task_count);

    for (const target*& pt : pts)
    {
      if (pt == nullptr) // Skipped.
        continue;

      target_state s (
        execute_async (
          a, *pt, target::count_busy (), t.task_count));

      if (s == target_state::postponed)
      {
        rs |= s;
        pt = nullptr;
      }
      else if (s == target_state::busy)
        mark (pt);
    }

    wg.wait ();

    bool e (mt == timestamp_nonexistent);
    const target* rt (tt != nullptr ? nullptr : &t);

    for (const target*& pt : pts)
    {
      if (pt == nullptr)
        continue;

      // If the target was already busy, wait for its completion.
      //
      if (unmark (pt))
        sched.wait (
          target::count_executed (), pt->task_count, scheduler::work_none);

      target_state s (pt->executed_state ());
      rs |= s;

      // Should we compare the timestamp to this target's?
      //
      if (!e && (!pf || pf (*pt)))
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (auto mpt = dynamic_cast<const mtime_target*> (pt))
        {
          timestamp mp (mpt->mtime ());

          // The same logic as in mtime_target::newer() (but avoids a call to
          // state()).
          //
          if (mt < mp || (mt == mp && s == target_state::changed))
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

      if (rt == nullptr && pt->is_a (*tt))
        rt = pt;
    }

    assert (rt != nullptr);

    return pair<optional<target_state>, const target*> (
      e ? optional<target_state> () : rs,
      tt != nullptr ? rt : nullptr);
  }

  target_state
  noop_action (action a, const target& t)
  {
    text << "noop action triggered for " << diag_doing (a, t);
    assert (false); // We shouldn't be called, see target::recipe().
    return target_state::unchanged;
  }

  target_state
  group_action (action a, const target& t)
  {
    // If the group is busy, we wait, similar to prerequisites.
    //
    const target& g (*t.group);

    if (execute (a, g) == target_state::busy)
      sched.wait (
        target::count_executed (), g.task_count, scheduler::work_none);

    // Indicate to execute() that this target's state comes from the group
    // (which, BTW, can be failed).
    //
    return target_state::group;
  }

  target_state
  default_action (action a, const target& t)
  {
    return execute_prerequisites (a, t);
  }

  target_state
  clean_extra (action a,
               const file& ft,
               initializer_list<initializer_list<const char*>> extra)
  {
    // Clean the extras first and don't print the commands at verbosity level
    // below 3. Note the first extra file/directory that actually got removed
    // for diagnostics below.
    //
    target_state er (target_state::unchanged);
    bool ed (false);
    path ep;

    auto clean = [&er, &ed, &ep] (const file& f,
                                  const path* fp,
                                  initializer_list<const char*> es)
    {
      for (const char* e: es)
      {
        size_t n;
        if (e == nullptr || (n = strlen (e)) == 0)
          continue;

        path p;
        bool d;

        if (path::traits::absolute (e))
        {
          p = path (e);
          d = p.to_directory ();
        }
        else
        {
          if ((d = (e[n - 1] == '/')))
            --n;

          if (fp == nullptr)
          {
            fp = &f.path ();
            assert (!fp->empty ()); // Must be assigned.
          }

          p = *fp;
          for (; *e == '-'; ++e)
            p = p.base ();

          p.append (e, n);
        }

        target_state r (target_state::unchanged);

        if (d)
        {
          dir_path dp (path_cast<dir_path> (p));

          switch (build2::rmdir_r (dp, true, 3))
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
          if (rmfile (p, 3))
            r = target_state::changed;
        }

        if (r == target_state::changed && ep.empty ())
        {
          ed = d;
          ep = move (p);
        }

        er |= r;
      }
    };

    auto ei (extra.begin ()), ee (extra.end ());

    if (ei != ee)
      clean (ft, nullptr, *ei++);

    // Now clean the ad hoc group file members, if any.
    //
    for (const target* m (ft.member); m != nullptr; m = m->member)
    {
      const file* fm (dynamic_cast<const file*> (m));
      const path* fp (fm != nullptr ? &fm->path () : nullptr);

      if (fm == nullptr || fp->empty ())
        continue;

      if (ei != ee)
        clean (*fm, fp, *ei++);

      target_state r (rmfile (*fp, 3)
                      ? target_state::changed
                      : target_state::unchanged);

      if (r == target_state::changed && ep.empty ())
        ep = *fp;

      er |= r;
    }

    // Now clean the primary target and its prerequisited in the reverse order
    // of update: first remove the file, then clean the prerequisites.
    //
    target_state tr (rmfile (ft.path (), ft) // Path must be assigned.
                     ? target_state::changed
                     : target_state::unchanged);

    // Update timestamp in case there are operations after us that could use
    // the information.
    //
    ft.mtime (timestamp_nonexistent);

    // Clean prerequisites.
    //
    tr |= reverse_execute_prerequisites (a, ft);

    // Factor the result of removing the extra files into the target state.
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
      if (verb > 0 && verb < 3)
      {
        if (ed)
          text << "rm -r " << path_cast<dir_path> (ep);
        else
          text << "rm " << ep;
      }
    }

    tr |= er;
    return tr;
  }

  target_state
  perform_clean (action a, const target& t)
  {
    return clean_extra (a, dynamic_cast<const file&> (t), {nullptr});
  }

  target_state
  perform_clean_depdb (action a, const target& t)
  {
    return clean_extra (a, dynamic_cast<const file&> (t), {".d"});
  }
}
