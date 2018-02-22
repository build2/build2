// file      : build2/algorithm.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/algorithm.hxx>

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/rule.hxx>
#include <build2/file.hxx> // import()
#include <build2/search.hxx>
#include <build2/context.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>
#include <build2/prerequisite.hxx>

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

  const target*
  search_existing (const prerequisite_key& pk)
  {
    assert (phase == run_phase::match || phase == run_phase::execute);

    return pk.proj ? import_existing (pk) : search_existing_target (pk);
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

  // target_lock
  //
#ifdef __cpp_thread_local
  thread_local
#else
  __thread
#endif
  const target_lock* target_lock::stack = nullptr;

  // If the work_queue is absent, then we don't wait.
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
        phase_unlock ul;
        e = sched.wait (busy - 1, task_count, *wq);
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
    assert (phase == run_phase::match);

    atomic_count& task_count (t[a].task_count);

    // Set the task count and wake up any threads that might be waiting for
    // this target.
    //
    task_count.store (offset + target::count_base (), memory_order_release);
    sched.resume (task_count);
  }

  target_lock
  add_adhoc_member (action a, target& t, const target_type& tt, const char* s)
  {
    string n (t.name);
    if (s != nullptr)
    {
      n += '.';
      n += s;
    }

    const_ptr<target>* mp (&t.member);
    for (; *mp != nullptr && !(*mp)->is_a (tt); mp = &(*mp)->member) ;

    const target& m (*mp != nullptr // Might already be there.
                     ? **mp
                     : search (t, tt, t.dir, t.out, n));

    target_lock l (lock (a, m));
    assert (l.target != nullptr); // Someone messing with ad hoc members?

    if (*mp == nullptr)
      *mp = l.target;
    else
      assert ((*mp)->name == n); // Basic sanity check.

    return l;
  };

  // Return the matching rule or NULL if no match and try_match is true.
  //
  const rule_match*
  match_impl (action a, target& t, const rule* skip, bool try_match)
  {
    // If this is an outer operation (Y-for-X), then we look for rules
    // registered for the outer id (X). Note that we still pass the original
    // action to the rule's match() function so that it can distinguish
    // between a pre/post operation (Y-for-X) and the actual operation (X).
    //
    meta_operation_id mo (a.meta_operation ());
    operation_id o (a.inner () ? a.operation () : a.outer_operation ());

    const scope& bs (t.base_scope ());

    for (auto tt (&t.type ()); tt != nullptr; tt = tt->base)
    {
      // Search scopes outwards, stopping at the project root.
      //
      for (const scope* s (&bs);
           s != nullptr;
           s = s->root () ? global_scope : s->parent_scope ())
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

          const auto& rules (i->second); // Hint map.

          // @@ TODO
          //
          // Different rules can be used for different operations (update vs
          // test is a good example). So, at some point, we will probably have
          // to support a list of hints or even an operation-hint map (e.g.,
          // 'hint=cxx test=foo' if cxx supports the test operation but we
          // want the foo rule instead). This is also the place where the
          // '{build clean}=cxx' construct (which we currently do not support)
          // can come handy.
          //
          // Also, ignore the hint (that is most likely ment for a different
          // operation) if this is a unique match.
          //
          string hint;
          auto rs (rules.size () == 1
                   ? make_pair (rules.begin (), rules.end ())
                   : rules.find_sub (hint));

          for (auto i (rs.first); i != rs.second; ++i)
          {
            const auto& r (*i);
            const string& n (r.first);
            const rule& ru (r.second);

            if (&ru == skip)
              continue;

            {
              auto df = make_diag_frame (
                [a, &t, &n](const diag_record& dr)
                {
                  if (verb != 0)
                    dr << info << "while matching rule " << n << " to "
                       << diag_do (a, t);
                });

              if (!ru.match (a, t, hint))
                continue;
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
                if (!ru1.match (a, t, hint))
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
              return &r;
            else
              dr << info << "use rule hint to disambiguate this match";
          }
        }
      }
    }

    if (!try_match)
    {
      diag_record dr;
      dr << fail << "no rule to " << diag_do (a, t);

      if (verb < 4)
        dr << info << "re-run with --verbose 4 for more information";
    }

    return nullptr;
  }

  recipe
  apply_impl (action a,
              target& t,
              const pair<const string, reference_wrapper<const rule>>& r)
  {
    auto df = make_diag_frame (
      [a, &t, &r](const diag_record& dr)
      {
        if (verb != 0)
          dr << info << "while applying rule " << r.first << " to "
             << diag_do (a, t);
      });

    return r.second.get ().apply (a, t);
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

          // Clear the resolved targets list and the data pad before calling
          // match(). The rule is free to modify these in its match()
          // (provided that it matches) in order to, for example, convey some
          // information to apply().
          //
          t.prerequisite_targets[a].clear ();
          if (a.inner ()) t.clear_data ();

          const rule_match* r (match_impl (a, t, nullptr, try_match));

          if (r == nullptr) // Not found (try_match == true).
          {
            l.offset = target::offset_tried;
            return make_pair (false, target_state::unknown);
          }

          s.rule = r;
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
      t.prerequisite_targets[a].clear ();
      if (a.inner ()) t.clear_data ();

      s.state = target_state::failed;
      l.offset = target::offset_applied;
    }

    return make_pair (true, s.state);
  }

  // If try_match is true, then indicate whether there is a rule match with
  // the first half of the result.
  //
  pair<bool, target_state>
  match (action a,
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

    if (l.target == nullptr)
    {
      // Already applied, executed, or busy.
      //
      if (l.offset >= target::offset_busy)
        return make_pair (true, target_state::busy);

      // Fall through.
    }
    else
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
      if (sched.async (start_count,
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
                           phase_lock pl (run_phase::match); // Can throw.
                           {
                             target_lock l {a, &t, offset}; // Reassemble.
                             match_impl (l, false /* step */, try_match);
                             // Unlock within the match phase.
                           }
                         }
                         catch (const failed&) {} // Phase lock failure.
                       },
                       diag_frame::stack,
                       target_lock::stack,
                       ref (*ld.target),
                       ld.offset))
        return make_pair (true, target_state::postponed); // Queued.

      // Matched synchronously, fall through.
    }

    return ct.try_matched_state (a, false);
  }

  group_view
  resolve_members_impl (action a, const target& g, target_lock l)
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
        // @@ What if we always do match & execute together? After all,
        //    if a group can be resolved in apply(), then it can be
        //    resolved in match()!
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

  void
  resolve_group_impl (action, const target&, target_lock l)
  {
    match_impl (l, true /* step */, true /* try_match */);
  }

  template <typename R>
  static void
  match_prerequisite_range (action a, target& t, R&& r, const scope* s)
  {
    auto& pts (t.prerequisite_targets[a]);

    // Start asynchronous matching of prerequisites. Wait with unlocked phase
    // to allow phase switching.
    //
    wait_guard wg (target::count_busy (), t[a].task_count, true);

    size_t i (pts.size ()); // Index of the first to be added.
    for (auto&& p: forward<R> (r))
    {
      const target& pt (search (t, p));

      if (s != nullptr && !pt.in (*s))
        continue;

      match_async (a, pt, target::count_busy (), t[a].task_count);
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

  template <typename T>
  void
  match_members (action a, target& t, T const* ts, size_t n)
  {
    // Pretty much identical to match_prerequisite_range() except we don't
    // search.
    //
    wait_guard wg (target::count_busy (), t[a].task_count, true);

    for (size_t i (0); i != n; ++i)
    {
      const target* m (ts[i]);

      if (m == nullptr || marked (m))
        continue;

      match_async (a, *m, target::count_busy (), t[a].task_count);
    }

    wg.wait ();

    // Finish matching all the targets that we have started.
    //
    for (size_t i (0); i != n; ++i)
    {
      const target* m (ts[i]);

      if (m == nullptr || marked (m))
        continue;

      match (a, *m);
    }
  }

  // Instantiate only for what we need.
  //
  template void
  match_members<const target*> (action, target&, const target* const*, size_t);

  template void
  match_members<prerequisite_target> (
    action, target&, prerequisite_target const*, size_t);

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
    else
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
      match (a, *r);
      t.prerequisite_targets[a].emplace_back (r);
    }

    return r;
  }

  // Execute the specified recipe (if any) and the scope operation callbacks
  // (if any/applicable) then merge and return the resulting target state.
  //
  static target_state
  execute_recipe (action a, target& t, const recipe& r)
  {
    target_state ts (target_state::unknown);

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
        op_s = &scopes.find (t.dir);

        if (op_s->out_path () == t.dir && !op_s->operation_callbacks.empty ())
        {
          op_p = op_s->operation_callbacks.equal_range (a);

          if (op_p.first == op_p.second)
            op_s = nullptr; // Ignore.
        }
        else
          op_s = nullptr; // Ignore.
      }

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
      ts |= r != nullptr ? r (a, t) : target_state::unchanged;

      // Post operations.
      //
      if (op_s != nullptr)
      {
        for (auto i (op_p.first); i != op_p.second; ++i)
          if (const auto& f = i->second.post)
            ts |= f (a, *op_s, *op_t);
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

  static target_state
  execute_impl (action a, target& t)
  {
    target::opstate& s (t[a]);

    assert (s.task_count.load (memory_order_consume) == target::count_busy ()
            && s.state == target_state::unknown);

    target_state ts (execute_recipe (a, t, s.recipe));

    // Decrement the target count (see set_recipe() for details).
    //
    if (a.inner ())
    {
      recipe_function** f (s.recipe.target<recipe_function*> ());
      if (f == nullptr || *f != &group_action)
        target_count.fetch_sub (1, memory_order_relaxed);
    }

    // Decrement the task count (to count_executed) and wake up any threads
    // that might be waiting for this target.
    //
    size_t tc (s.task_count.fetch_sub (
                 target::offset_busy - target::offset_executed,
                 memory_order_release));
    assert (tc == target::count_busy ());
    sched.resume (s.task_count);

    return ts;
  }

  target_state
  execute (action a,
           const target& ct,
           size_t start_count,
           atomic_count* task_count)
  {
    target& t (const_cast<target&> (ct)); // MT-aware.
    target::opstate& s (t[a]);

    // Update dependency counts and make sure they are not skew.
    //
    size_t gd (dependency_count.fetch_sub (1, memory_order_relaxed));
    size_t td (s.dependents.fetch_sub (1, memory_order_release));
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

    // Try to atomically change applied to busy.
    //
    size_t tc (target::count_applied ());

    size_t exec (target::count_executed ());
    size_t busy (target::count_busy ());

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
        sched.resume (s.task_count);
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
                         [a] (const diag_frame* ds, target& t)
                         {
                           diag_frame::stack_guard dsg (ds);
                           execute_impl (a, t);
                         },
                         diag_frame::stack,
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
  execute_direct (action a, const target& ct)
  {
    target& t (const_cast<target&> (ct)); // MT-aware.
    target::opstate& s (t[a]);

    // Similar logic to match() above except we execute synchronously.
    //
    size_t tc (target::count_applied ());

    size_t exec (target::count_executed ());
    size_t busy (target::count_busy ());

    if (s.task_count.compare_exchange_strong (
          tc,
          busy,
          memory_order_acq_rel,  // Synchronize on success.
          memory_order_acquire)) // Synchronize on failure.
    {
      if (s.state == target_state::unchanged)
      {
        if (t.is_a<dir> ())
          execute_recipe (a, t, nullptr /* recipe */);

        s.task_count.store (exec, memory_order_release);
        sched.resume (s.task_count);
      }
      else
        execute_impl (a, t);
    }
    else
    {
        // If the target is busy, wait for it.
        //
        if (tc >= busy) sched.wait (exec, s.task_count, scheduler::work_none);
        else            assert (tc == exec);
    }

    return t.executed_state (a);
  }

  template <typename T>
  target_state
  straight_execute_members (action a, atomic_count& tc,
                            T ts[], size_t n, size_t p)
  {
    target_state r (target_state::unchanged);

    // Start asynchronous execution of prerequisites.
    //
    wait_guard wg (target::count_busy (), tc);

    n += p;
    for (size_t i (p); i != n; ++i)
    {
      const target*& mt (ts[i]);

      if (mt == nullptr) // Skipped.
        continue;

      target_state s (execute_async (a, *mt, target::count_busy (), tc));

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

      // If the target is still busy, wait for its completion.
      //
      const auto& tc (mt[a].task_count);
      if (tc.load (memory_order_acquire) >= target::count_busy ())
        sched.wait (target::count_executed (), tc, scheduler::work_none);

      r |= mt.executed_state (a);
    }

    return r;
  }

  template <typename T>
  target_state
  reverse_execute_members (action a, atomic_count& tc,
                           T ts[], size_t n, size_t p)
  {
    // Pretty much as straight_execute_members() but in reverse order.
    //
    target_state r (target_state::unchanged);

    wait_guard wg (target::count_busy (), tc);

    n = p - n;
    for (size_t i (p); i != n; )
    {
      const target*& mt (ts[--i]);

      if (mt == nullptr)
        continue;

      target_state s (execute_async (a, *mt, target::count_busy (), tc));

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

      const auto& tc (mt[a].task_count);
      if (tc.load (memory_order_acquire) >= target::count_busy ())
        sched.wait (target::count_executed (), tc, scheduler::work_none);

      r |= mt.executed_state (a);
    }

    return r;
  }

  // Instantiate only for what we need.
  //
  template target_state
  straight_execute_members<const target*> (
    action, atomic_count&, const target*[], size_t, size_t);

  template target_state
  reverse_execute_members<const target*> (
    action, atomic_count&, const target*[], size_t, size_t);

  template target_state
  straight_execute_members<prerequisite_target> (
    action, atomic_count&, prerequisite_target[], size_t, size_t);

  template target_state
  reverse_execute_members<prerequisite_target> (
    action, atomic_count&, prerequisite_target[], size_t, size_t);

  pair<optional<target_state>, const target*>
  execute_prerequisites (const target_type* tt,
                         action a, const target& t,
                         const timestamp& mt, const prerequisite_filter& pf,
                         size_t n)
  {
    assert (current_mode == execution_mode::first);

    auto& pts (t.prerequisite_targets[a]);

    if (n == 0)
      n = pts.size ();

    // Pretty much as straight_execute_members() but hairier.
    //
    target_state rs (target_state::unchanged);

    wait_guard wg (target::count_busy (), t[a].task_count);

    for (size_t i (0); i != n; ++i)
    {
      const target*& pt (pts[i]);

      if (pt == nullptr) // Skipped.
        continue;

      target_state s (
        execute_async (
          a, *pt, target::count_busy (), t[a].task_count));

      if (s == target_state::postponed)
      {
        rs |= s;
        pt = nullptr;
      }
    }

    wg.wait ();

    bool e (mt == timestamp_nonexistent);
    const target* rt (tt != nullptr ? nullptr : &t);

    for (size_t i (0); i != n; ++i)
    {
      if (pts[i] == nullptr)
        continue;

      const target& pt (*pts[i]);

      const auto& tc (pt[a].task_count);
      if (tc.load (memory_order_acquire) >= target::count_busy ())
        sched.wait (target::count_executed (), tc, scheduler::work_none);

      target_state s (pt.executed_state (a));
      rs |= s;

      // Should we compare the timestamp to this target's?
      //
      if (!e && (!pf || pf (pt, i)))
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (const mtime_target* mpt = pt.is_a<mtime_target> ())
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

      if (rt == nullptr && pt.is_a (*tt))
        rt = &pt;
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
    assert (false); // We shouldn't be called (see set_recipe()).
    return target_state::unchanged;
  }

  target_state
  group_action (action a, const target& t)
  {
    // If the group is busy, we wait, similar to prerequisites.
    //
    const target& g (*t.group);

    if (execute (a, g) == target_state::busy)
      sched.wait (target::count_executed (),
                  g[a].task_count,
                  scheduler::work_none);

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

    auto clean_extra = [&er, &ed, &ep] (const file& f,
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
      clean_extra (ft, nullptr, *ei++);

    // Check if we were asked not to actually remove the files. The extras are
    // tricky: some of them, like depdb should definitely be removed. But
    // there could also be those that shouldn't. Currently we only use this
    // for auto-generated source code where the only extra file, if any, is
    // depdb so for now we treat them as "to remove" but in the future we may
    // need to have two lists.
    //
    bool clean (cast_true<bool> (ft[var_clean]));

    // Now clean the ad hoc group file members, if any.
    //
    for (const target* m (ft.member); m != nullptr; m = m->member)
    {
      const file* fm (m->is_a<file> ());
      const path* fp (fm != nullptr ? &fm->path () : nullptr);

      if (fm == nullptr || fp->empty ())
        continue;

      if (ei != ee)
        clean_extra (*fm, fp, *ei++);

      target_state r (clean && rmfile (*fp, 3)
                      ? target_state::changed
                      : target_state::unchanged);

      if (r == target_state::changed && ep.empty ())
        ep = *fp;

      er |= r;
    }

    // Now clean the primary target and its prerequisited in the reverse order
    // of update: first remove the file, then clean the prerequisites.
    //
    target_state tr (clean && rmfile (ft.path (), ft) // Path must be assigned.
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
      if (verb > (current_diag_noise ? 0 : 1) && verb < 3)
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
    return clean_extra (a, t.as<file> (), {nullptr});
  }

  target_state
  perform_clean_depdb (action a, const target& t)
  {
    return clean_extra (a, t.as<file> (), {".d"});
  }

  target_state
  perform_clean_group (action a, const target& xg)
  {
    const mtime_target& g (xg.as<mtime_target> ());

    // Similar logic to clean_extra() above.
    //
    target_state r (target_state::unchanged);

    if (cast_true<bool> (g[var_clean]))
    {
      for (group_view gv (g.group_members (a)); gv.count != 0; --gv.count)
      {
        if (const target* m = gv.members[gv.count - 1])
        {
          if (rmfile (m->as<file> ().path (), *m))
            r |= target_state::changed;
        }
      }
    }

    g.mtime (timestamp_nonexistent);

    r |= reverse_execute_prerequisites (a, g);
    return r;
  }
}
