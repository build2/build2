// file      : libbuild2/test/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/rule.hxx>

#ifndef _WIN32
#  include <signal.h>                  // SIG*
#else
#  include <libbutl/win32-utility.hxx> // DBG_TERMINATE_PROCESS
#endif

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/test/target.hxx>

#include <libbuild2/test/script/parser.hxx>
#include <libbuild2/test/script/runner.hxx>
#include <libbuild2/test/script/script.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    bool rule::
    match (action, target&) const
    {
      // We always match, even if this target is not testable (so that we can
      // ignore it; see apply()).
      //
      return true;
    }

    recipe rule::
    apply (action a, target& t) const
    {
      // Note that we are called both as the outer part during the update-for-
      // test pre-operation and as the inner part during the test operation
      // itself.
      //
      // In both cases we first determine if the target is testable and return
      // noop if it's not. Otherwise, in the first case (update for test) we
      // delegate to the normal update and in the second (test) -- perform the
      // test.
      //
      // And to add a bit more complexity, we want to handle aliases slightly
      // differently: we may not want to ignore their prerequisites if the
      // alias is not testable since their prerequisites could be.
      //
      // Here is the state matrix:
      //
      //                         test'able      |  pass'able  |  neither
      //                                        |             |
      // update-for-test     delegate (& pass)  |    pass     |   noop
      // ---------------------------------------+-------------+---------
      //            test     test     (& pass)  |    pass     |   noop
      //
      auto& pts (t.prerequisite_targets[a]);

      // Resolve group members.
      //
      if (!see_through_only || t.type ().see_through ())
      {
        // Remember that we are called twice: first during update for test
        // (pre-operation) and then during test. During the former, we rely on
        // the normal update rule to resolve the group members. During the
        // latter, there will be no rule to do this but the group will already
        // have been resolved by the pre-operation.
        //
        // If the rule could not resolve the group, then we ignore it.
        //
        group_view gv (a.outer ()
                       ? resolve_members (a, t)
                       : t.group_members (a));

        if (gv.members != nullptr)
        {
          for (size_t i (0); i != gv.count; ++i)
          {
            if (const target* m = gv.members[i])
              pts.push_back (m);
          }

          match_members (a, t, pts);
        }
      }

      // If we are passing-through, then match our prerequisites.
      //
      if (t.is_a<alias> () && pass (t))
      {
        // For the test operation we have to implement our own search and
        // match because we need to ignore prerequisites that are outside of
        // our project. They can be from projects that don't use the test
        // module (and thus won't have a suitable rule). Or they can be from
        // no project at all (e.g., installed). Also, generally, not testing
        // stuff that's not ours seems right.
        //
        // At least that was the thinking until we've added support for ad hoc
        // importation and the ability to "pull" other project's targets in a
        // "glue" kind of project. Also, on the other hand to the above
        // reasoning, it is unlikely a "foreign" target is listed as a
        // prerequisite of an alias unintentionally. For example, an alias is
        // unlikely to depend on an installed header or library. So now we
        // allow this.
        //
        match_prerequisites (a, t);
      }

      size_t pass_n (pts.size ()); // Number of pass-through prerequisites.

      // See if it's testable and if so, what kind.
      //
      bool test   (false);
      bool script (false);

      if (this->test (t))
      {
        // We have two very different cases: testscript and simple test (plus
        // it may not be a testable target at all). So as the first step
        // determine which case this is.
        //
        // If we have any prerequisites of the testscript{} type, then this is
        // the testscript case.
        //
        // If we can, go inside see-through groups. Normally groups won't be
        // resolvable for this action but then normally they won't contain any
        // testscripts either. In other words, if there is a group that
        // contains testscripts as members then it will need to arrange for
        // the members to be resolvable (e.g., by registering an appropriate
        // rule for the test operation).
        //
        for (prerequisite_member p:
               group_prerequisite_members (a, t, members_mode::maybe))
        {
          if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
            continue;

          if (p.is_a<testscript> ())
          {
            if (!script)
            {
              script = true;

              // We treat this target as testable unless the test variable is
              // explicitly set to false.
              //
              const name* n (cast_null<name> (t[var_test]));
              test = (n == nullptr || !n->simple () || n->value != "false");

              if (!test)
                break;
            }

            // Collect testscripts after the pass-through prerequisites.
            //
            const target& pt (p.search (t));

            // Note that for the test operation itself we don't match nor
            // execute them relying on update to assign their paths.
            //
            // Causing update for test inputs/scripts is tricky: we cannot
            // match for update-for-install because this same rule will match
            // and since the target is not testable, it will return the noop
            // recipe.
            //
            // So what we are going to do is directly match (and also execute;
            // see below) a recipe for the inner update (who thought we could
            // do that... but it seems we can). While at first it might feel
            // iffy, it does make sense: the outer rule we would have matched
            // would have simply delegated to the inner so we might as well
            // take a shortcut. The only potential drawback of this approach
            // is that we won't be able to provide any for-test customizations
            // when updating test inputs/scripts. But such a need seems rather
            // far fetched.
            //
            if (a.operation () == update_id)
              match_inner (a, pt);

            pts.push_back (&pt);
          }
        }

        // If this is not a script, then determine if it is a simple test.
        // Ignore testscript files themselves at the outset.
        //
        if (!script && !t.is_a<testscript> ())
        {
          // For the simple case whether this is a test is controlled by the
          // test variable. Also, it feels redundant to specify, say, "test =
          // true" and "test.stdout = test.out" -- the latter already says this
          // is a test.
          //
          const name* n (cast_null<name> (t[var_test]));

          // If the test variable is explicitly set to false then we treat
          // it as not testable regardless of what other test.* variables
          // or prerequisites we might have.
          //
          // Note that the test variable can be set to an "override" target
          // (which means 'true' for our purposes).
          //
          if (n != nullptr && n->simple () && n->value == "false")
            test = false;
          else
          {
            // Look for test input/stdin/stdout prerequisites. The same group
            // reasoning as in the testscript case above.
            //
            for (prerequisite_member p:
                   group_prerequisite_members (a, t, members_mode::maybe))
            {
              const auto& vars (p.prerequisite.vars);

              if (vars.empty ()) // Common case.
                continue;

              if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
                continue;

              bool rt (      cast_false<bool> (vars[test_roundtrip]));
              bool si (rt || cast_false<bool> (vars[test_stdin]));
              bool so (rt || cast_false<bool> (vars[test_stdout]));
              bool in (      cast_false<bool> (vars[test_input]));

              if (si || so || in)
              {
                // Verify it is file-based.
                //
                if (!p.is_a<file> ())
                {
                  fail << "test." << (si ? "stdin" : so ? "stdout" : "input")
                       << " prerequisite " << p << " of target " << t
                       << " is not a file";
                }

                if (!test)
                {
                  test = true;

                  // First matching prerequisite. Establish the structure in
                  // pts: the first element (after pass_n) is stdin (can be
                  // NULL), the second is stdout (can be NULL), and everything
                  // after that (if any) is inputs.
                  //
                  pts.push_back (nullptr); // stdin
                  pts.push_back (nullptr); // stdout
                }

                // Collect them after the pass-through prerequisites.
                //
                // Note that for the test operation itself we don't match nor
                // execute them relying on update to assign their paths.
                //
                auto match = [a, &p, &t] () -> const target*
                {
                  const target& pt (p.search (t));

                  // The same match_inner() rationale as for the testcript
                  // prerequisites above.
                  //
                  if (a.operation () == update_id)
                    match_inner (a, pt);

                  return &pt;
                };

                if (si)
                {
                  if (pts[pass_n] != nullptr)
                    fail << "multiple test.stdin prerequisites for target "
                         << t;

                  pts[pass_n] = match ();
                }

                if (so)
                {
                  if (pts[pass_n + 1] != nullptr)
                    fail << "multiple test.stdout prerequisites for target "
                         << t;

                  pts[pass_n + 1] = match ();
                }

                if (in)
                  pts.push_back (match ());
              }
            }

            if (!test)
              test = (n != nullptr); // We have the test variable.

            if (!test)
              test = t[test_options] || t[test_arguments];
          }
        }
      }

      // Neither testing nor passing-through.
      //
      if (!test && pass_n == 0)
        return noop_recipe;

      // If we are only passing-through, then use the default recipe (which
      // will execute all the matched prerequisites).
      //
      if (!test)
        return default_recipe;

      // Being here means we are definitely testing and maybe passing-through.
      //
      if (a.operation () == update_id)
      {
        // For the update pre-operation match the inner rule (actual update).
        //
        match_inner (a, t);

        return [pass_n] (action a, const target& t)
        {
          return perform_update (a, t, pass_n);
        };
      }
      else
      {
        if (script)
        {
          return [pass_n, this] (action a, const target& t)
          {
            return perform_script (a, t, pass_n);
          };
        }
        else
        {
          return [pass_n, this] (action a, const target& t)
          {
            return perform_test (a, t, pass_n);
          };
        }
      }
    }

    target_state rule::
    perform_update (action a, const target& t, size_t pass_n)
    {
      // First execute the inner recipe then execute prerequisites.
      //
      target_state ts (execute_inner (a, t));

      if (pass_n != 0)
        ts |= straight_execute_prerequisites (a, t, pass_n);

      ts |= straight_execute_prerequisites_inner (a, t, 0, pass_n);

      return ts;
    }

    static script::scope_state
    perform_script_impl (const target& t,
                         const testscript& ts,
                         const dir_path& wd,
                         const common& c)
    {
      using namespace script;

      scope_state r;

      try
      {
        build2::test::script::script s (t, ts, wd);

        {
          parser p (t.ctx);
          p.pre_parse (s);

          default_runner r (c);
          p.execute (s, r);
        }

        r = s.state;
      }
      catch (const failed&)
      {
        r = scope_state::failed;
      }

      return r;
    }

    target_state rule::
    perform_script (action a, const target& t, size_t pass_n) const
    {
      context& ctx (t.ctx);

      // First pass through.
      //
      if (pass_n != 0)
        straight_execute_prerequisites (a, t, pass_n);

      // Figure out whether the testscript file is called 'testscript', in
      // which case it should be the only one.
      //
      auto& pts (t.prerequisite_targets[a]);
      size_t pts_n (pts.size ());

      bool one;
      {
        optional<bool> o;
        for (size_t i (pass_n); i != pts_n; ++i)
        {
          const testscript& ts (*pts[i]->is_a<testscript> ());

          bool r (ts.name == "testscript");

          if ((r && o) || (!r && o && *o))
            fail << "both 'testscript' and other names specified for " << t;

          o = r;
        }

        assert (o); // We should have a testscript or we wouldn't be here.
        one = *o;
      }

      // Calculate root working directory. It is in the out_base of the target
      // and is called just test for dir{} targets and test-<target-name> for
      // other targets.
      //
      dir_path wd (t.out_dir ());

      if (t.is_a<dir> ())
        wd /= "test";
      else
        wd /= "test-" + t.name;

      // Are we backlinking the test working directory to src? (See
      // backlink_*() in algorithm.cxx for details.)
      //
      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());
      const path& buildignore_file (rs.root_extra->buildignore_file);

      dir_path bl;
      if (cast_false<bool> (rs.vars[ctx.var_forwarded]))
      {
        bl = bs.src_path () / wd.leaf (bs.out_path ());
        clean_backlink (ctx, bl, verb_never);
      }

      // If this is a (potentially) multi-testscript test, then create (and
      // later cleanup) the root directory. If this is just 'testscript', then
      // the root directory is used directly as test's working directory and
      // it's the runner's responsibility to create and clean it up.
      //
      // Note that we create the root directory containing the .buildignore
      // file to make sure that it is ignored by name patterns (see the
      // buildignore description for details).
      //
      // What should we do if the directory already exists? We used to fail
      // which meant the user had to go and clean things up manually every
      // time a test failed. This turned out to be really annoying. So now we
      // issue a warning and clean it up automatically. The drawbacks of this
      // approach are the potential loss of data from the previous failed test
      // run and the possibility of deleting user-created files.
      //
      if (exists (static_cast<const path&> (wd), false))
        fail << "working directory " << wd << " is a file/symlink";

      if (exists (wd))
      {
        if (before != output_before::clean)
        {
          bool fail (before == output_before::fail);

          (fail ? error : warn) << "working directory " << wd << " exists "
                                << (empty_buildignore (wd, buildignore_file)
                                    ? ""
                                    : "and is not empty ")
                                << "at the beginning of the test";

          if (fail)
            throw failed ();
        }

        // Remove the directory itself not to confuse the runner which tries
        // to detect when tests stomp on each others feet.
        //
        rmdir_r (ctx, wd, true, 2);
      }

      // Delay actually creating the directory in case all the tests are
      // ignored (via config.test).
      //
      bool mk (!one);

      // Start asynchronous execution of the testscripts.
      //
      wait_guard wg;

      if (!ctx.dry_run)
        wg = wait_guard (ctx, ctx.count_busy (), t[a].task_count);

      // Result vector.
      //
      using script::scope_state;

      vector<scope_state> res;
      res.reserve (pts_n - pass_n); // Make sure there are no reallocations.

      for (size_t i (pass_n); i != pts_n; ++i)
      {
        const testscript& ts (*pts[i]->is_a<testscript> ());

        // If this is just the testscript, then its id path is empty (and it
        // can only be ignored by ignoring the test target, which makes sense
        // since it's the only testscript file).
        //
        if (one || test (t, path (ts.name)))
        {
          // Because the creation of the output directory is shared between us
          // and the script implementation (plus the fact that we actually
          // don't clean the existing one), we are going to ignore it for
          // dry-run.
          //
          if (!ctx.dry_run)
          {
            if (mk)
            {
              mkdir_buildignore (ctx, wd, buildignore_file, 2);
              mk = false;
            }
          }

          if (verb)
          {
            // If the target is an alias, then testscript itself is the
            // target.
            //
            if (t.is_a<alias> ())
              print_diag ("test", ts);
            else
            {
              // In this case the test is really a combination of the target
              // and testscript and using "->" feels off. Also, let's list the
              // testscript after the target even though its a source.
              //
              print_diag ("test", t, ts, "+");
            }
          }

          res.push_back (ctx.dry_run
                         ? scope_state::passed
                         : scope_state::unknown);

          if (!ctx.dry_run)
          {
            scope_state& r (res.back ());

            if (!ctx.sched->async (ctx.count_busy (),
                                   t[a].task_count,
                                   [this] (const diag_frame* ds,
                                           scope_state& r,
                                           const target& t,
                                           const testscript& ts,
                                           const dir_path& wd)
                                   {
                                     diag_frame::stack_guard dsg (ds);
                                     r = perform_script_impl (t, ts, wd, *this);
                                   },
                                   diag_frame::stack (),
                                   ref (r),
                                   cref (t),
                                   cref (ts),
                                   cref (wd)))
            {
              // Executed synchronously. If failed and we were not asked to
              // keep going, bail out.
              //
              if (r == scope_state::failed && !ctx.keep_going)
                break;
            }
          }
        }
      }

      if (!ctx.dry_run)
        wg.wait ();

      // Re-examine.
      //
      bool bad (false);
      for (scope_state r: res)
      {
        switch (r)
        {
        case scope_state::passed:              break;
        case scope_state::failed:  bad = true; break;
        case scope_state::unknown: assert (false);
        }

        if (bad)
          break;
      }

      // Cleanup.
      //
      if (!ctx.dry_run)
      {
        if (!bad && !one && !mk && after == output_after::clean)
        {
          if (!empty_buildignore (wd, buildignore_file))
            fail << "working directory " << wd << " is not empty at the "
                 << "end of the test";

          rmdir_buildignore (ctx, wd, buildignore_file, 2);
        }
      }

      // Backlink if the working directory exists.
      //
      // If we dry-run then presumably all tests passed and we shouldn't
      // have anything left unless we are keeping the output.
      //
      if (!bl.empty () && (ctx.dry_run
                           ? after == output_after::keep
                           : exists (wd)))
        update_backlink (ctx, wd, bl, true /* changed */);

      if (bad)
        throw failed ();

      return target_state::changed;
    }

    // The format of args shall be:
    //
    // name1 arg arg ... nullptr
    // name2 arg arg ... nullptr
    // ...
    // nameN arg arg ... nullptr nullptr
    //
    // Stack-allocated linked list of information about the running pipeline
    // processes.
    //
    // Note: constructed incrementally.
    //
    struct pipe_process
    {
      // Initially NULL. Set to the address of the process object when it is
      // created. Reset back to NULL when the process is executed and its exit
      // status is collected (see complete_pipe() for details).
      //
      process* proc = nullptr;

      char const** args; // Only for diagnostics.

      diag_buffer dbuf;
      bool force_dbuf;

      // True if this process has been terminated.
      //
      bool terminated = false;

      // True if this process has been terminated but we failed to read out
      // its stderr stream in the reasonable timeframe (2 seconds) after the
      // termination.
      //
      // Note that this may happen if there is a still running child process
      // of the terminated process which has inherited the parent's stderr
      // file descriptor.
      //
      bool unread_stderr = false;

      pipe_process* prev; // NULL for the left-most program.
      pipe_process* next; // Left-most program for the right-most program.

      pipe_process (context& x,
                    char const** as,
                    bool fb,
                    pipe_process* p,
                    pipe_process* f)
          : args (as), dbuf (x), force_dbuf (fb), prev (p), next (f) {}
    };

    static void
    run_test (const target& t,
              char const** args,
              int ofd,
              const optional<timestamp>& deadline,
              pipe_process* prev = nullptr)
    {
      // Find the next process, if any.
      //
      char const** next (args);
      for (next++; *next != nullptr; next++) ;
      next++;

      bool last (*next == nullptr);

      // Redirect stdout to a pipe unless we are last.
      //
      int out (last ? ofd : -1);

      // Propagate the pointer to the left-most program.
      //
      // Also force diag buffering for the trailing diff process, so it's
      // stderr is never printed if the test program fails (see
      // complete_pipe() for details).
      //
      pipe_process pp (t.ctx,
                       args,
                       last && ofd == 2,
                       prev,
                       prev != nullptr ? prev->next : nullptr);

      if (prev != nullptr)
        prev->next = &pp;
      else
        pp.next = &pp; // Points to itself.

      try
      {
        // Wait for a process to complete until the deadline is reached and
        // return the underlying wait function result.
        //
        auto timed_wait = [] (process& p, const timestamp& deadline)
        {
          timestamp now (system_clock::now ());
          return deadline > now
                 ? p.timed_wait (deadline - now)
                 : p.try_wait ();
        };

        // Terminate the pipeline processes starting from the specified one
        // and up to the leftmost one and then kill those which didn't
        // terminate in 2 seconds. Issue diagnostics and fail if something
        // goes wrong, but still try to terminate all processes.
        //
        auto term_pipe = [&timed_wait] (pipe_process* pp)
        {
          diag_record dr;

          // Terminate processes gracefully and set the terminate flag for
          // them.
          //
          for (pipe_process* p (pp); p != nullptr; p = p->prev)
          {
            try
            {
              p->proc->term ();
            }
            catch (const process_error& e)
            {
              dr << fail << "unable to terminate " << p->args[0] << ": " << e;
            }

            p->terminated = true;
          }

          // Wait a bit for the processes to terminate and kill the remaining
          // ones.
          //
          timestamp deadline (system_clock::now () + chrono::seconds (2));

          for (pipe_process* p (pp); p != nullptr; p = p->prev)
          {
            process& pr (*p->proc);

            try
            {
              if (!timed_wait (pr, deadline))
              {
                pr.kill ();
                pr.wait ();
              }
            }
            catch (const process_error& e)
            {
              dr << fail << "unable to wait/kill " << p->args[0] << ": " << e;
            }
          }
        };

        // Read out all the pipeline's buffered strerr streams watching for
        // the deadline, if specified. If the deadline is reached, then
        // terminate the whole pipeline, move the deadline by another 2
        // seconds, and continue reading.
        //
        // Note that we assume that this timeout increment is normally
        // sufficient to read out the buffered data written by the already
        // terminated processes. If, however, that's not the case (see
        // pipe_process for the possible reasons), then we just set
        // unread_stderr flag to true for such processes and bail out.
        //
        // Also note that this implementation is inspired by the
        // script::run_pipe::read_pipe() lambda.
        //
        auto read_pipe = [&pp, &deadline, &term_pipe] ()
        {
          fdselect_set fds;
          for (pipe_process* p (&pp); p != nullptr; p = p->prev)
          {
            diag_buffer& b (p->dbuf);

            if (b.is.is_open ())
              fds.emplace_back (b.is.fd (), p);
          }

          optional<timestamp> dl (deadline);
          bool terminated (false);

          for (size_t unread (fds.size ()); unread != 0;)
          {
            try
            {
              // If a deadline is specified, then pass the timeout to
              // fdselect().
              //
              if (dl)
              {
                timestamp now (system_clock::now ());

                if (*dl <= now || ifdselect (fds, *dl - now) == 0)
                {
                  if (!terminated)
                  {
                    term_pipe (&pp);
                    terminated = true;

                    dl = system_clock::now () + chrono::seconds (2);
                    continue;
                  }
                  else
                  {
                    for (fdselect_state& s: fds)
                    {
                      if (s.fd != nullfd)
                      {
                        pipe_process* p (static_cast<pipe_process*> (s.data));

                        p->unread_stderr = true;

                        // Let's also close the stderr stream not to confuse
                        // diag_buffer::close() (see script::read() for
                        // details).
                        //
                        try
                        {
                          p->dbuf.is.close ();
                        }
                        catch (const io_error&) {}
                      }
                    }

                    break;
                  }
                }
              }
              else
                ifdselect (fds);

              for (fdselect_state& s: fds)
              {
                if (s.ready)
                {
                  pipe_process* p (static_cast<pipe_process*> (s.data));

                  if (!p->dbuf.read (p->force_dbuf))
                  {
                    s.fd = nullfd;
                    --unread;
                  }
                }
              }
            }
            catch (const io_error& e)
            {
              fail << "io error reading pipeline streams: " << e;
            }
          }
        };

        // Wait for the pipeline processes to complete, watching for the
        // deadline, if specified. If the deadline is reached, then terminate
        // the whole pipeline.
        //
        // Note: must be called after read_pipe().
        //
        auto wait_pipe = [&pp, &deadline, &timed_wait, &term_pipe] ()
        {
          for (pipe_process* p (&pp); p != nullptr; p = p->prev)
          {
            try
            {
              if (!deadline)
                p->proc->wait ();
              else if (!timed_wait (*p->proc, *deadline))
                term_pipe (p);
            }
            catch (const process_error& e)
            {
              fail << "unable to wait " << p->args[0] << ": " << e;
            }
          }
        };

        // Iterate over the pipeline processes left to right, printing their
        // stderr if buffered and issuing the diagnostics if the exit code is
        // not available (terminated abnormally or due to a deadline), is
        // non-zero, or stderr was not fully read. Afterwards, fail if any of
        // such a faulty processes were encountered.
        //
        // Note that we only issue diagnostics for the first failure.
        //
        // Note: must be called after wait_pipe() and only once.
        //
        auto complete_pipe = [&pp, &t] ()
        {
          pipe_process* b (pp.next); // Left-most program.
          assert (b != nullptr);     // The lambda can only be called once.
          pp.next = nullptr;

          bool fail (false);
          for (pipe_process* p (b); p != nullptr; p = p->next)
          {
            assert (p->proc != nullptr); // The lambda can only be called once.

            // Collect the exit status, if present.
            //
            // Absent if the process misses the deadline.
            //
            optional<process_exit> pe;

            const process& pr (*p->proc);

#ifndef _WIN32
            if (!(p->terminated       &&
                  !pr.exit->normal () &&
                  pr.exit->signal () == SIGTERM))
#else
            if (!(p->terminated       &&
                  !pr.exit->normal () &&
                  pr.exit->status == DBG_TERMINATE_PROCESS))
#endif
              pe = pr.exit;

            p->proc = nullptr;

            // Verify the exit status and issue the diagnostics on failure.
            //
            // Note that we only issue diagnostics for the first failure but
            // continue iterating to reset process pointers to NULL. Also note
            // that if the test program fails, then the potential diff's
            // diagnostics is suppressed since it is always buffered.
            //
            if (!fail)
            {
              diag_record dr;

              // Note that there can be a race, so that the process we have
              // terminated due to reaching the deadline has in fact exited
              // normally. Thus, the 'unread stderr' situation can also happen
              // to a successfully terminated process. If that's the case, we
              // report this problem as the main error and the secondary error
              // otherwise.
              //
              if (!pe              ||
                  !pe->normal ()   ||
                  pe->code () != 0 ||
                  p->unread_stderr)
              {
                fail = true;

                dr << error << "test " << t << " failed" // Multi test: test 1.
                   << error << "process " << p->args[0] << ' ';

                if (!pe)
                {
                  dr << "terminated: execution timeout expired";

                  if (p->unread_stderr)
                    dr << error << "stderr not closed after exit";
                }
                else if (!pe->normal () || pe->code () != 0)
                {
                   dr << *pe;

                  if (p->unread_stderr)
                    dr << error << "stderr not closed after exit";
                }
                else
                {
                  assert (p->unread_stderr);

                  dr << "stderr not closed after exit";
                }

                if (verb == 1)
                {
                  dr << info << "test command line: ";

                  for (pipe_process* p (b); p != nullptr; p = p->next)
                  {
                    if (p != b)
                      dr << " | ";

                    print_process (dr, p->args);
                  }
                }
              }

              // Now print the buffered stderr, if present, and/or flush the
              // diagnostics, if issued.
              //
              if (p->dbuf.is_open ())
                p->dbuf.close (move (dr));
            }
          }

          if (fail)
            throw failed ();
        };

        process p;
        {
          process::pipe ep;
          {
            fdpipe p;
            if (diag_buffer::pipe (t.ctx, pp.force_dbuf) == -1) // Buffering?
            {
              try
              {
                p = fdopen_pipe ();
              }
              catch (const io_error& e)
              {
                fail << "unable to redirect stderr: " << e;
              }

              // Note that we must return non-owning fd to our end of the pipe
              // (see the process class for details).
              //
              ep = process::pipe (p.in.get (), move (p.out));
            }
            else
              ep = process::pipe (-1, 2);

            // Note that we must open the diag buffer regardless of the
            // diag_buffer::pipe() result.
            //
            pp.dbuf.open (args[0], move (p.in), fdstream_mode::non_blocking);
          }

          p = (prev == nullptr
               ? process (args, 0, out, move (ep))             // First process.
               : process (args, *prev->proc, out, move (ep))); // Next process.
        }

        pp.proc = &p;

        // If the right-hand part of the pipe fails, then make sure we don't
        // wait indefinitely in the process destructor if the deadline is
        // specified or just because a process is blocked on stderr.
        //
        auto g (make_exception_guard ([&pp, &term_pipe] ()
        {
          if (pp.proc != nullptr)
          try
          {
            // Close all buffered pipeline stderr streams ignoring io_error
            // exceptions.
            //
            for (pipe_process* p (&pp); p != nullptr; p = p->prev)
            {
              if (p->dbuf.is.is_open ())
              try
              {
                p->dbuf.is.close();
              }
              catch (const io_error&) {}
            }

            term_pipe (&pp);
          }
          catch (const failed&)
          {
            // We can't do much here.
          }
        }));

        if (!last)
          run_test (t, next, ofd, deadline, &pp);

        // Complete the pipeline execution, if not done yet.
        //
        if (pp.proc != nullptr)
        {
          read_pipe ();
          wait_pipe ();
          complete_pipe ();
        }
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child)
          exit (1);

        throw failed ();
      }
    }

    target_state rule::
    perform_test (action a, const target& tt, size_t pass_n) const
    {
      context& ctx (tt.ctx);

      // First pass through.
      //
      if (pass_n != 0)
        straight_execute_prerequisites (a, tt, pass_n);

      // See if we have the test executable override.
      //
      path p;
      {
        // Note that the test variable's visibility is target.
        //
        lookup l (tt[var_test]);

        // Note that we have similar code for scripted tests.
        //
        const target* t (nullptr);

        if (l.defined ())
        {
          const name* n (cast_null<name> (l));

          if (n == nullptr)
            fail << "invalid test executable override: null value";
          else if (n->empty ())
            fail << "invalid test executable override: empty value";
          else if (n->simple ())
          {
            // Ignore the special 'true' value.
            //
            if (n->value != "true")
              p = path (n->value);
            else
              t = &tt;
          }
          else if (n->directory ())
            fail << "invalid test executable override: '" << *n << "'";
          else
          {
            // Must be a target name. Could be from src (e.g., a script).
            //
            // @@ OUT: what if this is a @-qualified pair of names?
            //
            t = search_existing (*n, tt.base_scope ());

            if (t == nullptr)
              fail << "invalid test executable override: unknown target: '"
                   << *n << "'";
          }
        }
        else
          // By default we set it to the test target's path.
          //
          t = &tt;

        if (t != nullptr)
        {
          if (auto* pt = t->is_a<path_target> ())
          {
            // Do some sanity checks: the target better be up-to-date with
            // an assigned path.
            //
            p = pt->path ();

            if (p.empty ())
              fail << "target " << *pt << " specified in the test variable "
                   << "is out of date" <<
                info << "consider specifying it as a prerequisite of " << tt;
          }
          else
            fail << "target " << *t << (t != &tt
                                        ? " specified in the test variable "
                                        : " requested to be tested ")
                 << "is not path-based";
        }
      }

      // See apply() for the structure of prerequisite_targets in the presence
      // of test.{input,stdin,stdout}.
      //
      auto& pts (tt.prerequisite_targets[a]);
      size_t pts_n (pts.size ());

      cstrings args;

      // Do we have stdin?
      //
      // We simulate stdin redirect (<file) with a fake (already terminate)
      // cat pipe (cat file |).
      //
      bool sin (pass_n != pts_n && pts[pass_n] != nullptr);

      process cat;
      if (sin)
      {
        const file& it (pts[pass_n]->as<file> ());
        const path& ip (it.path ());
        assert (!ip.empty ()); // Should have been assigned by update.

        cat = process (process_exit (0)); // Successfully exited.

        if (!ctx.dry_run)
        {
          try
          {
            cat.in_ofd = fdopen (ip, fdopen_mode::in);
          }
          catch (const io_error& e)
          {
            fail << "unable to open " << ip << ": " << e;
          }
        }

        // Purely for diagnostics.
        //
        args.push_back ("cat");
        args.push_back (ip.string ().c_str ());
        args.push_back (nullptr);
      }

      process_path pp;

      // Do we have a test runner?
      //
      if (runner_path == nullptr)
      {
        // If dry-run, the target may not exist.
        //
        pp = process_path (!ctx.dry_run
                           ? run_search     (p, true /* init */)
                           : run_try_search (p, true));

        args.push_back (pp.empty ()
                        ? p.string ().c_str ()
                        : pp.recall_string ());
      }
      else
      {
        args.push_back (runner_path->recall_string ());

        append_options (args, *runner_options);

        // Leave it to the runner to resolve the test program path.
        //
        args.push_back (p.string ().c_str ());
      }

      // Do we have options and/or arguments?
      //
      if (auto l = tt[test_options])
        append_options (args, cast<strings> (l));

      if (auto l = tt[test_arguments])
        append_options (args, cast<strings> (l));

      // Do we have inputs?
      //
      for (size_t i (pass_n + 2); i < pts_n; ++i)
      {
        const file& it (pts[i]->as<file> ());
        const path& ip (it.path ());
        assert (!ip.empty ()); // Should have been assigned by update.
        args.push_back (ip.string ().c_str ());
      }

      args.push_back (nullptr);

      // Do we have stdout?
      //
      // If we do, then match it using diff. Also redirect the diff's stdout
      // to stderr, similar to how we do that for the script (see
      // script::check_output() for the reasoning). That will also prevent the
      // diff's output from interleaving with any other output.
      //
      path dp ("diff");
      process_path dpp;
      int ofd (1);

      if (pass_n != pts_n && pts[pass_n + 1] != nullptr)
      {
        ofd = 2;

        const file& ot (pts[pass_n + 1]->as<file> ());
        const path& op (ot.path ());
        assert (!op.empty ()); // Should have been assigned by update.

        dpp = run_search (dp, true);

        args.push_back (dpp.recall_string ());
        args.push_back ("-u");

        // Note that MinGW-built diff utility (as of 3.3) fails trying to
        // detect if stdin contains text or binary data. We will help it a bit
        // to workaround the issue.
        //
#ifdef _WIN32
        args.push_back ("--text");
#endif

        // Ignore Windows newline fluff if that's what we are running on.
        //
        if (cast<target_triplet> (tt[test_target]).class_ == "windows")
          args.push_back ("--strip-trailing-cr");

        const char* f (op.string ().c_str ());

        // Note that unmatched program stdout will be referred by diff as '-'
        // by default. Let's name it as 'stdout' for clarity and consistency
        // with the buildscript diagnostics.
        //
        // Also note that the -L option is not portable but is supported by all
        // the major implementations (see script/run.cxx for details).
        //
        args.push_back ("-L");
        args.push_back (f);

        args.push_back ("-L");
        args.push_back ("stdout");

        args.push_back (f);
        args.push_back ("-");
        args.push_back (nullptr);
      }

      args.push_back (nullptr); // Second.

      if (verb >= 2)
        print_process (args); // Note: prints the whole pipeline.
      else if (verb)
        print_diag ("test", tt);

      if (!ctx.dry_run)
      {
        pipe_process pp (tt.ctx,
                         args.data (), // Note: only cat's args are considered.
                         false /* force_dbuf */,
                         nullptr /* prev */,
                         nullptr /* next */);

        if (sin)
        {
          pp.next = &pp;  // Points to itself.
          pp.proc = &cat;
        }

        run_test (tt,
                  args.data () + (sin ? 3 : 0), // Skip cat.
                  ofd,
                  test_deadline (tt),
                  sin ? &pp : nullptr);
      }

      return target_state::changed;
    }
  }
}
