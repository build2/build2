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
#include <build2/scheduler>
#include <build2/filesystem>
#include <build2/diagnostics>
#include <build2/prerequisite>

using namespace std;
using namespace butl;

namespace build2
{
  target&
  search (const prerequisite_key& pk)
  {
    assert (phase == run_phase::search_match);

    // If this is a project-qualified prerequisite, then this is import's
    // business.
    //
    if (pk.proj)
      return import (pk);

    if (target* t = pk.tk.type->search (pk))
      return *t;

    return create_new_target (pk);
  }

  target&
  search (name n, const scope& s)
  {
    assert (phase == run_phase::search_match);

    optional<string> ext;
    const target_type* tt (s.find_target_type (n, ext));

    if (tt == nullptr)
      fail << "unknown target type " << n.type << " in name " << n;

    if (!n.dir.empty ())
      n.dir.normalize (false, true); // Current dir collapses to an empty one.

    // @@ OUT: for now we assume the prerequisite's out is undetermined.
    //         Would need to pass a pair of names.
    //
    return search (*tt,
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
    assert (phase == run_phase::search_match || phase == run_phase::execute);

    // We don't handle this for now.
    //
    if (cn.qualified ())
      return nullptr;

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

    // @@ OUT: for now we assume the prerequisite's out is undetermined.
    //         Would need to pass a pair of names.
    //
    return search_existing_target (
      prerequisite_key {n.proj, {tt, &n.dir, &out, &n.value, ext}, &s});
  }

  pair<const rule*, match_result>
  match_impl (slock& ml, action a, target& t, bool apply, const rule* skip)
  {
    pair<const rule*, match_result> r (nullptr, false);

    // By default, clear the resolved targets list before calling
    // match(). The rule is free to modify this list in match()
    // (provided that it matches) in order to, for example, prepare
    // it for apply().
    //
    t.reset (a);

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
              const string& n (i->first);
              const rule& ru (i->second);

              if (&ru == skip)
                continue;

              match_result m (false);
              {
                auto g (
                  make_exception_guard (
                    [ra, &t, &n]()
                    {
                      info << "while matching rule " << n << " to "
                           << diag_do (ra, t);
                    }));

                if (!(m = ru.match (ml, ra, t, hint)))
                  continue;

                if (!m.recipe_action.valid ())
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
                  auto g (
                    make_exception_guard (
                      [ra, &t, &n1]()
                      {
                        info << "while matching rule " << n1 << " to "
                             << diag_do (ra, t);
                      }));

                  if (!ru1.match (ml, ra, t, hint))
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
              {
                ra = m.recipe_action; // Use custom, if set.

                if (apply)
                {
                  auto g (
                    make_exception_guard (
                      [ra, &t, &n]()
                      {
                        info << "while applying rule " << n << " to "
                             << diag_do (ra, t);
                      }));

                  // @@ We could also allow the rule to change the recipe
                  // action in apply(). Could be useful with delegates.
                  //
                  t.recipe (ra, ru.apply (ml, ra, t));
                }
                else
                {
                  r.first = &ru;
                  r.second = move (m);
                }

                return r;
              }
              else
                dr << info << "use rule hint to disambiguate this match";
            }
          }
        }
      }
    }

    diag_record dr;
    dr << fail << "no rule to " << diag_do (a, t);

    if (verb < 4)
      dr << info << "re-run with --verbose 4 for more information";

    dr << endf;
  }

  group_view
  resolve_group_members_impl (slock& ml, action a, target& g)
  {
    group_view r;

    // Unless we already have a recipe, try matching the target to
    // the rule.
    //
    if (!g.recipe (a))
    {
      auto rp (match_impl (ml, a, g, false));

      r = g.group_members (a);
      if (r.members != nullptr)
        return r;

      // That didn't help, so apply the rule and go to the building
      // phase.
      //
      const match_result& mr (rp.second);
      g.recipe (mr.recipe_action,
                rp.first->apply (ml, mr.recipe_action, g));
    }

    // Note that we use execute_direct() rather than execute() here to
    // sidestep the dependents count logic. In this context, this is by
    // definition the first attempt to execute this rule (otherwise we
    // would have already known the members list) and we really do need
    // to execute it now.
    //
    execute_direct (a, g);

    r = g.group_members (a);
    return r; // Might still be unresolved.
  }

  void
  search_and_match_prerequisites (slock& ml,
                                  action a,
                                  target& t,
                                  const scope* s)
  {
    for (prerequisite& p: group_prerequisites (t))
    {
      target& pt (search (p));

      if (s == nullptr || pt.in (*s))
      {
        match (ml, a, pt);
        t.prerequisite_targets.push_back (&pt);
      }
    }
  }

  void
  search_and_match_prerequisite_members (slock& ml,
                                         action a,
                                         target& t,
                                         const scope* s)
  {
    for (prerequisite_member p: group_prerequisite_members (ml, a, t))
    {
      target& pt (p.search ());

      if (s == nullptr || pt.in (*s))
      {
        match (ml, a, pt);
        t.prerequisite_targets.push_back (&pt);
      }
    }
  }

  fsdir*
  inject_fsdir (slock& ml, action a, target& t, bool parent)
  {
    tracer trace ("inject_fsdir");

    const scope& bs (t.base_scope ());
    const scope* rs (bs.root_scope ());

    // Handle the outside of any project case. Note that we also used to bail
    // our of this is the root of the project. But that proved not to be such
    // a great idea in case of subprojects (e.g., tests/).
    //
    if (rs == nullptr)
      return nullptr;

    // If t is a directory (name is empty), say foo/bar/, then t is bar and
    // its parent directory is foo/.
    //
    const dir_path& d (parent && t.name.empty () ? t.dir.directory () : t.dir);

    // Handle the src = out.
    //
    if (d.sub (rs->src_path ()))
      return nullptr;

    l6 ([&]{trace << d << " for " << t;});

    // Target is in the out tree, so out directory is empty.
    //
    fsdir* r (&search<fsdir> (d, dir_path (), string (), nullptr, nullptr));
    match (ml, a, *r);
    t.prerequisite_targets.emplace_back (r);
    return r;
  }

  target_state
  execute_impl (action a, target& t) noexcept
  {
    // Task count should be count_executing.
    //
    assert (t.state_ == target_state::unknown);

    target_state ts;

    try
    {
      ts = t.recipe (a) (a, t);

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
      // The "how we got here" stack trace is useful but only during serial
      // execution in the "stop going" mode. Otherwise, you not only get
      // things interleaved, you may also get a whole bunch of such stacks.
      //
      if (verb != 0 && sched.serial () && !keep_going)
        info << "while " << diag_doing (a, t);

      ts = t.state_ = target_state::failed;
    }

    // Decrement the task count (to count_executed) and wake up any threads
    // that might be waiting for this target.
    //
    size_t tc (t.task_count--);
    assert (tc == target::count_executing);
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

    // text << "E " << t << ": " << t.dependents << " " << dependency_count;

    // Update dependency counts and make sure they are not skew.
    //
    size_t td (t.dependents--);
#ifndef NDEBUG
    size_t gd (dependency_count--);
    assert (td != 0 && gd != 0);
#else
    dependency_count.fetch_sub (1, std::memory_order_release);
#endif
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

    // Try to atomically change unexecuted to executing.
    //
    size_t tc (target::count_unexecuted);
    if (t.task_count.compare_exchange_strong (tc, target::count_executing))
    {
      if (task_count == nullptr)
        return execute_impl (a, t);

      //text << this_thread::get_id () << " async " << t;

      if (sched.async (start_count,
                       *task_count,
                       [a] (target& t)
                       {
                         //text << this_thread::get_id () << " deque " << t;
                         execute_impl (a, t); // Note: noexcept.
                       },
                       ref (t)))
        return target_state::unknown; // Queued.

      // Executed synchronously, fall through.
    }
    else
    {
      switch (tc)
      {
      case target::count_unexecuted: assert (false);
      case target::count_executed:   break;
      default:                       return target_state::busy;
      }
    }

    return t.synchronized_state (false);
  }

  target_state
  execute_direct (action a, const target& ct)
  {
    target& t (const_cast<target&> (ct)); // MT-aware.

    size_t tc (target::count_unexecuted);
    if (t.task_count.compare_exchange_strong (tc, target::count_executing))
    {
      execute_impl (a, t);
    }
    else
    {
      // If the target is busy, wait for it.
      //
      switch (tc)
      {
      case target::count_unexecuted: assert (false);
      case target::count_executed: break;
      default: sched.wait (target::count_executed, t.task_count, false);
      }
    }

    return t.synchronized_state ();
  }

  // We use the last bit of a pointer to target in prerequisite_targets as a
  // flag to indicate whether the target was already busy. Probably not worth
  // it (we are saving an atomic load), but what the hell.
  //
  // VC15 doesn't like if we use (abstract) target here.
  //
  static_assert (alignof (file) % 2 == 0, "unexpected target alignment");

  static inline void
  set_busy (const target*& p)
  {
    uintptr_t i (reinterpret_cast<uintptr_t> (p));
    i |= 0x01;
    p = reinterpret_cast<const target*> (i);
  }

  static inline bool
  get_busy (const target*& p)
  {
    uintptr_t i (reinterpret_cast<uintptr_t> (p));

    if ((i & 0x01) != 0)
    {
      i &= ~uintptr_t (0x01);
      p = reinterpret_cast<const target*> (i);
      return true;
    }

    return false;
  }

  target_state
  straight_execute_members (action a,
                            const target& t,
                            const target* ts[],
                            size_t n)
  {
    target_state r (target_state::unchanged);

    // Start asynchronous execution of prerequisites keeping track of how many
    // we have handled.
    //
    size_t i (0);
    for (; i != n; ++i)
    {
      const target*& mt (ts[i]);

      if (mt == nullptr) // Skipped.
        continue;

      target_state s (
        execute_async (
          a, *mt, target::count_executing, t.task_count));

      if (s == target_state::postponed)
      {
        r |= s;
        mt = nullptr;
      }
      else if (s == target_state::busy)
        set_busy (mt);
      //
      // Bail out if the target has failed and we weren't instructed to
      // keep going.
      //
      else if (s == target_state::failed && !keep_going)
      {
        ++i;
        break;
      }
    }
    sched.wait (target::count_executing, t.task_count);

    // Now all the targets in prerequisite_targets must be executed and
    // synchronized (and we have blanked out all the postponed ones).
    //
    for (size_t j (0); j != i; ++j)
    {
      const target*& mt (ts[j]);

      if (mt == nullptr)
        continue;

      // If the target was already busy, wait for its completion.
      //
      if (get_busy (mt))
        sched.wait (target::count_executed, mt->task_count, false);

      r |= mt->synchronized_state (false);
    }

    if (r == target_state::failed)
      throw failed ();

    return r;
  }

  target_state
  reverse_execute_members (action a,
                           const target& t,
                           const target* ts[],
                           size_t n)
  {
    // Pretty much as straight_execute_members() but in reverse order.
    //
    target_state r (target_state::unchanged);

    size_t i (n);
    for (; i != 0; --i)
    {
      const target*& mt (ts[i - 1]);

      if (mt == nullptr)
        continue;

      target_state s (
        execute_async (
          a, *mt, target::count_executing, t.task_count));

      if (s == target_state::postponed)
      {
        r |= s;
        mt = nullptr;
      }
      else if (s == target_state::busy)
        set_busy (mt);
      else if (s == target_state::failed && !keep_going)
      {
        --i;
        break;
      }
    }
    sched.wait (target::count_executing, t.task_count);

    for (size_t j (n); j != i; --j)
    {
      const target*& mt (ts[j - 1]);

      if (mt == nullptr)
        continue;

      if (get_busy (mt))
        sched.wait (target::count_executed, mt->task_count, false);

      r |= mt->synchronized_state (false);
    }

    if (r == target_state::failed)
      throw failed ();

    return r;
  }

  pair<const target*, target_state>
  execute_prerequisites (const target_type* tt,
                         action a, const target& t,
                         const timestamp& mt, const prerequisite_filter& pf)
  {
    assert (current_mode == execution_mode::first);

    // Pretty much as straight_execute_members() but hairier.
    //
    target_state rs (target_state::unchanged);

    size_t i (0);
    for (size_t n (t.prerequisite_targets.size ()); i != n; ++i)
    {
      const target*& pt (t.prerequisite_targets[i]);

      if (pt == nullptr) // Skipped.
        continue;

      target_state s (
        execute_async (
          a, *pt, target::count_executing, t.task_count));

      if (s == target_state::postponed)
      {
        rs |= s;
        pt = nullptr;
      }
      else if (s == target_state::busy)
        set_busy (pt);
      else if (s == target_state::failed && !keep_going)
      {
        ++i;
        break;
      }
    }
    sched.wait (target::count_executing, t.task_count);

    bool e (mt == timestamp_nonexistent);
    const target* rt (tt != nullptr ? nullptr : &t);

    for (size_t j (0); j != i; ++j)
    {
      const target*& pt (t.prerequisite_targets[j]);

      if (pt == nullptr)
        continue;

      // If the target was already busy, wait for its completion.
      //
      if (get_busy (pt))
        sched.wait (target::count_executed, pt->task_count, false);

      target_state s (pt->synchronized_state (false));
      rs |= s;

      // Don't bother with the rest if we are failing.
      //
      if (rs == target_state::failed)
        continue;

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

    if (rs == target_state::failed)
      throw failed ();

    assert (rt != nullptr);
    return make_pair (e ? rt : nullptr, rs);
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
      sched.wait (target::count_executed, g.task_count, false);

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

          p = f.path ();
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
      clean (ft, *ei++);

    // Now clean the ad hoc group file members, if any.
    //
    for (const target* m (ft.member); m != nullptr; m = m->member)
    {
      const file* fm (dynamic_cast<const file*> (m));

      if (fm == nullptr || fm->path ().empty ())
        continue;

      if (ei != ee)
        clean (*fm, *ei++);

      const path& f (fm->path ());

      target_state r (rmfile (f, 3)
                      ? target_state::changed
                      : target_state::unchanged);

      if (r == target_state::changed && ep.empty ())
        ep = f;

      er |= r;
    }

    // Now clean the primary target and its prerequisited in the reverse order
    // of update: first remove the file, then clean the prerequisites.
    //
    target_state tr (rmfile (ft.path (), ft)
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
