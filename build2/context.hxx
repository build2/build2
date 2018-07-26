// file      : build2/context.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CONTEXT_HXX
#define BUILD2_CONTEXT_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/scope.hxx>
#include <build2/variable.hxx>
#include <build2/operation.hxx>
#include <build2/scheduler.hxx>

namespace build2
{
  // Main (and only) scheduler. Started up and shut down in main().
  //
  extern scheduler sched;

  // In order to perform each operation the build system goes through the
  // following phases:
  //
  // load     - load the buildfiles
  // match    - search prerequisites and match rules
  // execute  - execute the matched rule
  //
  // The build system starts with a "serial load" phase and then continues
  // with parallel search and execute. Match, however, can be interrupted
  // both with load and execute.
  //
  // Match can be interrupted with "exclusive load" in order to load
  // additional buildfiles. Similarly, it can be interrupted with (parallel)
  // execute in order to build targetd required to complete the match (for
  // example, generated source code or source code generators themselves).
  //
  // Such interruptions are performed by phase change that is protected by
  // phase_mutex (which is also used to synchronize the state changes between
  // phases).
  //
  // Serial load can perform arbitrary changes to the model. Exclusive load,
  // however, can only perform "island appends". That is, it can create new
  // "nodes" (variables, scopes, etc) but not (semantically) change already
  // existing nodes or invalidate any references to such (the idea here is
  // that one should be able to load additional buildfiles as long as they
  // don't interfere with the existing build state). The "islands" are
  // identified by the load_generation number (0 for the initial/serial
  // load). It is incremented in case of a phase switch and can be stored in
  // various "nodes" to verify modifications are only done "within the
  // islands".
  //
  extern run_phase phase;
  extern size_t load_generation;

  // A "tri-mutex" that keeps all the threads in one of the three phases. When
  // a thread wants to switch a phase, it has to wait for all the other
  // threads to do the same (or release their phase locks). The load phase is
  // exclusive.
  //
  // The interleaving match and execute is interesting: during match we read
  // the "external state" (e.g., filesystem entries, modifications times, etc)
  // and capture it in the "internal state" (our dependency graph). During
  // execute we are modifying the external state with controlled modifications
  // of the internal state to reflect the changes (e.g., update mtimes). If
  // you think about it, it's pretty clear that we cannot safely perform both
  // of these actions simultaneously. A good example would be running a code
  // generator and header dependency extraction simultaneously: the extraction
  // process may pick up headers as they are being generated. As a result, we
  // either have everyone treat the external state as read-only or write-only.
  //
  // There is also one more complication: if we are returning from a load
  // phase that has failed, then the build state could be seriously messed up
  // (things like scopes not being setup completely, etc). And once we release
  // the lock, other threads that are waiting will start relying on this
  // messed up state. So a load phase can mark the phase_mutex as failed in
  // which case all currently blocked and future lock()/relock() calls return
  // false. Note that in this case we still switch to the desired phase. See
  // the phase_{lock,switch,unlock} implementations for details.
  //
  class phase_mutex
  {
  public:
    // Acquire a phase lock potentially blocking (unless already in the
    // desired phase) until switching to the desired phase is possible.
    //
    bool
    lock (run_phase);

    // Release the phase lock potentially allowing (unless there are other
    // locks on this phase) switching to a different phase.
    //
    void
    unlock (run_phase);

    // Switch from one phase to another. Semantically, just unlock() followed
    // by lock() but more efficient.
    //
    bool
    relock (run_phase unlock, run_phase lock);

  private:
    friend struct phase_lock;
    friend struct phase_unlock;
    friend struct phase_switch;

    phase_mutex ()
        : fail_ (false), lc_ (0), mc_ (0), ec_ (0)
    {
      phase = run_phase::load;
    }

    static phase_mutex instance;

  private:
    // We have a counter for each phase which represents the number of threads
    // in or waiting for this phase.
    //
    // We use condition variables to wait for a phase switch. The load phase
    // is exclusive so we have a separate mutex to serialize it (think of it
    // as a second level locking).
    //
    // When the mutex is unlocked (all three counters become zero, the phase
    // is always changed to load (this is also the initial state).
    //
    mutex m_;

    bool fail_;

    size_t lc_;
    size_t mc_;
    size_t ec_;

    condition_variable lv_;
    condition_variable mv_;
    condition_variable ev_;

    mutex lm_;
  };

  // Grab a new phase lock releasing it on destruction. The lock can be
  // "owning" or "referencing" (recursive).
  //
  // On the referencing semantics: If there is already an instance of
  // phase_lock in this thread, then the new instance simply references it.
  //
  // The reason for this semantics is to support the following scheduling
  // pattern (in actual code we use wait_guard to RAII it):
  //
  // atomic_count task_count (0);
  //
  // {
  //   phase_lock l (run_phase::match);                    // (1)
  //
  //   for (...)
  //   {
  //     sched.async (task_count,
  //                  [] (...)
  //                  {
  //                    phase_lock pl (run_phase::match);  // (2)
  //                    ...
  //                  },
  //                  ...);
  //   }
  // }
  //
  // sched.wait (task_count);                              // (3)
  //
  // Here is what's going on here:
  //
  // 1. We first get a phase lock "for ourselves" since after the first
  //    iteration of the loop, things may become asynchronous (including
  //    attempts to switch the phase and modify the structure we are iteration
  //    upon).
  //
  // 2. The task can be queued or it can be executed synchronously inside
  //    async() (refer to the scheduler class for details on this semantics).
  //
  //    If this is an async()-synchronous execution, then the task will create
  //    a referencing phase_lock. If, however, this is a queued execution
  //    (including wait()-synchronous), then the task will create a top-level
  //    phase_lock.
  //
  //    Note that we only acquire the lock once the task starts executing
  //    (there is no reason to hold the lock while the task is sitting in the
  //    queue). This optimization assumes that whatever else we pass to the
  //    task (for example, a reference to a target) is stable (in other words,
  //    such a reference cannot become invalid).
  //
  // 3. Before calling wait(), we release our phase lock to allow switching
  //    the phase.
  //
  struct phase_lock
  {
    explicit phase_lock (run_phase);
    ~phase_lock ();

    phase_lock (phase_lock&&) = delete;
    phase_lock (const phase_lock&) = delete;

    phase_lock& operator= (phase_lock&&) = delete;
    phase_lock& operator= (const phase_lock&) = delete;

    run_phase p;

    static
#ifdef __cpp_thread_local
    thread_local
#else
    __thread
#endif
    phase_lock* instance;
  };

  // Assuming we have a lock on the current phase, temporarily release it
  // and reacquire on destruction.
  //
  struct phase_unlock
  {
    phase_unlock (bool unlock = true);
    ~phase_unlock () noexcept (false);

    phase_lock* l;
  };

  // Assuming we have a lock on the current phase, temporarily switch to a
  // new phase and switch back on destruction.
  //
  struct phase_switch
  {
    explicit phase_switch (run_phase);
    ~phase_switch () noexcept (false);

    run_phase o, n;
  };

  // Wait for a task count optionally and temporarily unlocking the phase.
  //
  struct wait_guard
  {
    ~wait_guard () noexcept (false);

    explicit
    wait_guard (atomic_count& task_count,
                bool phase = false);

    wait_guard (size_t start_count,
                atomic_count& task_count,
                bool phase = false);

    void
    wait ();

    size_t start_count;
    atomic_count* task_count;
    bool phase;
  };

  // Cached variables.
  //
  // Note: consider printing in info meta-operation if adding anything here.
  //
  extern const variable* var_src_root;
  extern const variable* var_out_root;
  extern const variable* var_src_base;
  extern const variable* var_out_base;
  extern const variable* var_forwarded;

  extern const variable* var_project;
  extern const variable* var_amalgamation;
  extern const variable* var_subprojects;
  extern const variable* var_version;

  extern const variable* var_project_url;     // project.url
  extern const variable* var_project_summary; // project.summary

  extern const variable* var_import_target;   // import.target

  extern const variable* var_clean;           // [bool] target visibility

  // Forwarded configuration backlink mode. Valid values are:
  //
  // false     - no link.
  // true      - make a link using appropriate mechanism.
  // symbolic  - make a symbolic link.
  // hard      - make a hard link.
  // copy      - make a copy.
  // overwrite - copy over but don't remove on clean (committed gen code).
  //
  extern const variable* var_backlink;        // [string] target visibility

  // Prerequisite inclusion/exclusion. Valid values are:
  //
  // false  - exclude.
  // true   - include.
  // adhoc  - include but treat as an ad hoc input.
  //
  // If a rule uses prerequisites as inputs (as opposed to just matching them
  // with the "pass-through" semantics), then the adhoc value signals that a
  // prerequisite is an ad hoc input. A rule should match and execute such a
  // prerequisite (whether its target type is recognized as suitable input or
  // not) and assume that the rest will be handled by the user (e.g., it will
  // be passed via a command line argument or some such). Note that this
  // mechanism can be used to both treat unknown prerequisite types as inputs
  // (for example, linker scripts) as well as prevent treatment of known
  // prerequisite types as such while still matching and executing them (for
  // example, plugin libraries).
  //
  // A rule with the "pass-through" semantics should treat the adhoc value
  // the same as true.
  //
  // To query this value in rule implementations use the include() helpers
  // from prerequisites.hxx.
  //
  extern const variable* var_include;         // [string] prereq visibility

  extern const char var_extension[10];        // "extension"

  // The build.* namespace.
  //
  extern const variable* var_build_meta_operation; // .meta_operation

  // Current action (meta/operation).
  //
  // The names unlike info are available during boot but may not yet be
  // lifted. The name is always for an outer operation (or meta operation
  // that hasn't been recognized as such yet).
  //
  extern string current_mname;
  extern string current_oname;

  extern const meta_operation_info* current_mif;
  extern const operation_info* current_inner_oif;
  extern const operation_info* current_outer_oif;
  extern size_t current_on; // Current operation number (1-based) in the
                            // meta-operation batch.

  extern execution_mode current_mode;

  // Some diagnostics (for example output directory creation/removal by the
  // fsdir rule) is just noise at verbosity level 1 unless it is the only
  // thing that is printed. So we can only suppress it in certain situations
  // (e.g., dist) where we know we have already printed something.
  //
  extern bool current_diag_noise;

  // Total number of dependency relationships and targets with non-noop
  // recipe in the current action.
  //
  // Together with target::dependents the dependency count is incremented
  // during the rule search & match phase and is decremented during execution
  // with the expectation of it reaching 0. Used as a sanity check.
  //
  // The target count is incremented after a non-noop recipe is matched and
  // decremented after such recipe has been executed. If such a recipe has
  // skipped executing the operation, then it should increment the skip count.
  // These two counters are used for progress monitoring and diagnostics.
  //
  extern atomic_count dependency_count;
  extern atomic_count target_count;
  extern atomic_count skip_count;

  inline void
  set_current_mif (const meta_operation_info& mif)
  {
    if (current_mname != mif.name)
    {
      current_mname = mif.name;
      global_scope->rw ().assign (var_build_meta_operation) = mif.name;
    }

    current_mif = &mif;
    current_on = 0; // Reset.
  }

  inline void
  set_current_oif (const operation_info& inner_oif,
                   const operation_info* outer_oif = nullptr,
                   bool diag_noise = true)
  {
    current_oname = (outer_oif == nullptr ? inner_oif : *outer_oif).name;
    current_inner_oif = &inner_oif;
    current_outer_oif = outer_oif;
    current_on++;
    current_mode = inner_oif.mode;
    current_diag_noise = diag_noise;

    // Reset counters (serial execution).
    //
    dependency_count.store (0, memory_order_relaxed);
    target_count.store (0, memory_order_relaxed);
    skip_count.store (0, memory_order_relaxed);
  }

  // Keep going flag.
  //
  // Note that setting it to false is not of much help unless we are running
  // serially. In parallel we queue most of the things up before we see any
  // failures.
  //
  extern bool keep_going;

  // Reset the build state. In particular, this removes all the targets,
  // scopes, and variables.
  //
  variable_overrides
  reset (const strings& cmd_vars);

  // Return the project name or empty string if unnamed.
  //
  inline const project_name&
  project (const scope& root)
  {
    auto l (root[var_project]);
    return l ? cast<project_name> (l) : empty_project_name;
  }

  // Return the src/out directory corresponding to the given out/src. The
  // passed directory should be a sub-directory of out/src_root.
  //
  dir_path
  src_out (const dir_path& out, const scope& root);

  dir_path
  src_out (const dir_path& out,
           const dir_path& out_root, const dir_path& src_root);

  dir_path
  out_src (const dir_path& src, const scope& root);

  dir_path
  out_src (const dir_path& src,
           const dir_path& out_root, const dir_path& src_root);

  // Action phrases, e.g., "configure update exe{foo}", "updating exe{foo}",
  // and "updating exe{foo} is configured". Use like this:
  //
  // info << "while " << diag_doing (a, t);
  //
  class target;

  struct diag_phrase
  {
    const action& a;
    const target& t;
    void (*f) (ostream&, const action&, const target&);
  };

  inline ostream&
  operator<< (ostream& os, const diag_phrase& p)
  {
    p.f (os, p.a, p.t);
    return os;
  }

  string
  diag_do (const action&);

  void
  diag_do (ostream&, const action&, const target&);

  inline diag_phrase
  diag_do (const action& a, const target& t)
  {
    return diag_phrase {a, t, &diag_do};
  }

  string
  diag_doing (const action&);

  void
  diag_doing (ostream&, const action&, const target&);

  inline diag_phrase
  diag_doing (const action& a, const target& t)
  {
    return diag_phrase {a, t, &diag_doing};
  }

  string
  diag_did (const action&);

  void
  diag_did (ostream&, const action&, const target&);

  inline diag_phrase
  diag_did (const action& a, const target& t)
  {
    return diag_phrase {a, t, &diag_did};
  }

  void
  diag_done (ostream&, const action&, const target&);

  inline diag_phrase
  diag_done (const action& a, const target& t)
  {
    return diag_phrase {a, t, &diag_done};
  }
}

#include <build2/context.ixx>

#endif // BUILD2_CONTEXT_HXX
