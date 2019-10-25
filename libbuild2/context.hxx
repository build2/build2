// file      : libbuild2/context.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONTEXT_HXX
#define LIBBUILD2_CONTEXT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

// NOTE: this file is included by pretty much every other build state header
//       (scope, target, variable, etc) so including any of them here is most
//       likely a non-starter.
//
#include <libbuild2/action.hxx>
#include <libbuild2/operation.hxx>
#include <libbuild2/scheduler.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  class context;

  class scope;
  class scope_map;
  class target_set;
  class target_type_map;

  class value;
  using values = small_vector<value, 1>;

  struct variable;
  class variable_pool;
  struct variable_override;
  using variable_overrides = vector<variable_override>;
  class variable_override_cache;

  class function_map;

  struct opspec;

  struct loaded_modules_lock;

  class LIBBUILD2_SYMEXPORT run_phase_mutex
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
    friend class context;

    run_phase_mutex (context& c)
      : ctx_ (c), fail_ (false), lc_ (0), mc_ (0), ec_ (0) {}

  private:
    friend struct phase_lock;
    friend struct phase_unlock;
    friend struct phase_switch;

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
    context& ctx_;

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

  // Context-wide mutexes and mutex shards.
  //
  class global_mutexes
  {
  public:

    // Variable cache mutex shard (see variable.hxx for details).
    //
    size_t                     variable_cache_size;
    unique_ptr<shared_mutex[]> variable_cache;

    explicit
    global_mutexes (size_t vc)
        : variable_cache_size (vc),
          variable_cache (new shared_mutex[variable_cache_size]) {}
  };

  // A build context encapsulates the state of a build. It is possible to have
  // multiple build contexts provided they are non-overlapping, that is, they
  // don't try to build the same projects (note that this is currently not
  // enforced).
  //
  // One context can be preempted to execute another context (we do this, for
  // example, to update build system modules). When switching to such a nested
  // context you may want to cutoff the diagnostics stack (and maybe insert
  // your own entry), for example:
  //
  //   diag_frame::stack_guard diag_cutoff (nullptr);
  //
  // As well as suppress progress which would otherwise clash (maybe in the
  // future we can do save/restore but then we would need some indication that
  // we have switched to another task).
  //
  // Note that sharing the same scheduler between multiple top-level contexts
  // can currently be problematic due to operation-specific scheduler tuning.
  //
  // The loaded_modules state (module.hxx) is shared among all the contexts
  // (there is no way to have multiple shared library loading "contexts") and
  // is protected by loaded_modules_lock. A nested context should normally
  // inherit this lock value from its outer context.
  //
  // Note also that any given thread should not participate in multiple
  // schedulers at the same time (see scheduler::join/leave() for details).
  //
  // @@ CTX TODO:
  //
  //   - Move verbosity level to context (see issue in import_module()).
  //
  //   - Scheduler tunning and multiple top-level contexts.
  //
  //   - Detect overlapping contexts (could be expensive).
  //
  class LIBBUILD2_SYMEXPORT context
  {
    struct data;
    unique_ptr<data> data_;

  public:
    scheduler& sched;
    global_mutexes& mutexes;

    // Dry run flag (see --dry-run|-n).
    //
    // This flag is set (based on dry_run_option) only for the final execute
    // phase (as opposed to those that interrupt match) by the perform meta
    // operation's execute() callback.
    //
    // Note that for this mode to function properly we have to use fake
    // mtimes. Specifically, a rule that pretends to update a target must set
    // its mtime to system_clock::now() and everyone else must use this cached
    // value. In other words, there should be no mtime re-query from the
    // filesystem. The same is required for "logical clean" (i.e., dry-run
    // 'clean update' in order to see all the command lines).
    //
    // At first, it may seem like we should also "dry-run" changes to depdb.
    // But that would be both problematic (some rules update it in apply()
    // during the match phase) and wasteful (why discard information). Also,
    // depdb may serve as an input to some commands (for example, to provide
    // C++ module mapping) which means that without updating it the commands
    // we print might not be runnable (think of the compilation database).
    //
    // One thing we need to be careful about if we are updating depdb is to
    // not render the target up-to-date. But in this case the depdb file will
    // be older than the target which in our model is treated as an
    // interrupted update (see depdb for details).
    //
    // Note also that sometimes it makes sense to do a bit more than
    // absolutely necessary or to discard information in order to keep the
    // rule logic sane.  And some rules may choose to ignore this flag
    // altogether. In this case, however, the rule should be careful not to
    // rely on functions (notably from filesystem) that respect this flag in
    // order not to end up with a job half done.
    //
    bool dry_run = false;
    bool dry_run_option;

    // Keep going flag.
    //
    // Note that setting it to false is not of much help unless we are running
    // serially: in parallel we queue most of the things up before we see any
    // failures.
    //
    bool keep_going;

    // In order to perform each operation the build system goes through the
    // following phases:
    //
    // load     - load the buildfiles
    // match    - search prerequisites and match rules
    // execute  - execute the matched rule
    //
    // The build system starts with a "serial load" phase and then continues
    // with parallel match and execute. Match, however, can be interrupted
    // both with load and execute.
    //
    // Match can be interrupted with "exclusive load" in order to load
    // additional buildfiles. Similarly, it can be interrupted with (parallel)
    // execute in order to build targetd required to complete the match (for
    // example, generated source code or source code generators themselves).
    //
    // Such interruptions are performed by phase change that is protected by
    // phase_mutex (which is also used to synchronize the state changes
    // between phases).
    //
    // Serial load can perform arbitrary changes to the build state. Exclusive
    // load, however, can only perform "island appends". That is, it can
    // create new "nodes" (variables, scopes, etc) but not (semantically)
    // change already existing nodes or invalidate any references to such (the
    // idea here is that one should be able to load additional buildfiles as
    // long as they don't interfere with the existing build state). The
    // "islands" are identified by the load_generation number (0 for the
    // initial/serial load). It is incremented in case of a phase switch and
    // can be stored in various "nodes" to verify modifications are only done
    // "within the islands".
    //
    run_phase phase = run_phase::load;
    size_t load_generation = 0;

    // A "tri-mutex" that keeps all the threads in one of the three phases.
    // When a thread wants to switch a phase, it has to wait for all the other
    // threads to do the same (or release their phase locks). The load phase
    // is exclusive.
    //
    // The interleaving match and execute is interesting: during match we read
    // the "external state" (e.g., filesystem entries, modifications times,
    // etc) and capture it in the "internal state" (our dependency graph).
    // During execute we are modifying the external state with controlled
    // modifications of the internal state to reflect the changes (e.g.,
    // update mtimes). If you think about it, it's pretty clear that we cannot
    // safely perform both of these actions simultaneously. A good example
    // would be running a code generator and header dependency extraction
    // simultaneously: the extraction process may pick up headers as they are
    // being generated. As a result, we either have everyone treat the
    // external state as read-only or write-only.
    //
    // There is also one more complication: if we are returning from a load
    // phase that has failed, then the build state could be seriously messed
    // up (things like scopes not being setup completely, etc). And once we
    // release the lock, other threads that are waiting will start relying on
    // this messed up state. So a load phase can mark the phase_mutex as
    // failed in which case all currently blocked and future lock()/relock()
    // calls return false. Note that in this case we still switch to the
    // desired phase. See the phase_{lock,switch,unlock} implementations for
    // details.
    //
    run_phase_mutex phase_mutex;

    // Current action (meta/operation).
    //
    // The names unlike info are available during boot but may not yet be
    // lifted. The name is always for an outer operation (or meta operation
    // that hasn't been recognized as such yet).
    //
    string current_mname;
    string current_oname;

    const meta_operation_info* current_mif;
    const operation_info* current_inner_oif;
    const operation_info* current_outer_oif;

    // Current operation number (1-based) in the meta-operation batch.
    //
    size_t current_on;

    // Note: we canote use the corresponding target::offeset_* values.
    //
    size_t count_base     () const {return 5 * (current_on - 1);}

    size_t count_touched  () const {return 1 + count_base ();}
    size_t count_tried    () const {return 2 + count_base ();}
    size_t count_matched  () const {return 3 + count_base ();}
    size_t count_applied  () const {return 4 + count_base ();}
    size_t count_executed () const {return 5 + count_base ();}
    size_t count_busy     () const {return 6 + count_base ();}

    // Execution mode.
    //
    execution_mode current_mode;

    // Some diagnostics (for example output directory creation/removal by the
    // fsdir rule) is just noise at verbosity level 1 unless it is the only
    // thing that is printed. So we can only suppress it in certain situations
    // (e.g., dist) where we know we have already printed something.
    //
    bool current_diag_noise;

    // Total number of dependency relationships and targets with non-noop
    // recipe in the current action.
    //
    // Together with target::dependents the dependency count is incremented
    // during the rule search & match phase and is decremented during
    // execution with the expectation of it reaching 0. Used as a sanity
    // check.
    //
    // The target count is incremented after a non-noop recipe is matched and
    // decremented after such recipe has been executed. If such a recipe has
    // skipped executing the operation, then it should increment the skip
    // count. These two counters are used for progress monitoring and
    // diagnostics.
    //
    atomic_count dependency_count;
    atomic_count target_count;
    atomic_count skip_count;

    // Build state (scopes, targets, variables, etc).
    //
    const scope_map& scopes;
    target_set& targets;
    const variable_pool& var_pool;
    const variable_overrides& var_overrides; // Project and relative scope.
    function_map& functions;

    // Global scope.
    //
    const scope& global_scope;
    const target_type_map& global_target_types;
    variable_override_cache& global_override_cache;
    const strings& global_var_overrides;

    // Cached variables.
    //

    // Note: consider printing in info meta-operation if adding anything here.
    //
    const variable* var_src_root;
    const variable* var_out_root;
    const variable* var_src_base;
    const variable* var_out_base;
    const variable* var_forwarded;

    const variable* var_project;
    const variable* var_amalgamation;
    const variable* var_subprojects;
    const variable* var_version;

    // project.url
    //
    const variable* var_project_url;

    // project.summary
    //
    const variable* var_project_summary;

    // import.*
    //
    const variable* var_import_build2;
    const variable* var_import_target;

    // [string] target visibility
    //
    const variable* var_extension;

    // [bool] target visibility
    //
    const variable* var_clean;

    // Forwarded configuration backlink mode. Valid values are:
    //
    // false     - no link.
    // true      - make a link using appropriate mechanism.
    // symbolic  - make a symbolic link.
    // hard      - make a hard link.
    // copy      - make a copy.
    // overwrite - copy over but don't remove on clean (committed gen code).
    //
    // Note that it can be set by a matching rule as a rule-specific variable.
    //
    // [string] target visibility
    //
    const variable* var_backlink;

    // Prerequisite inclusion/exclusion. Valid values are:
    //
    // false  - exclude.
    // true   - include.
    // adhoc  - include but treat as an ad hoc input.
    //
    // If a rule uses prerequisites as inputs (as opposed to just matching
    // them with the "pass-through" semantics), then the adhoc value signals
    // that a prerequisite is an ad hoc input. A rule should match and execute
    // such a prerequisite (whether its target type is recognized as suitable
    // input or not) and assume that the rest will be handled by the user
    // (e.g., it will be passed via a command line argument or some such).
    // Note that this mechanism can be used to both treat unknown prerequisite
    // types as inputs (for example, linker scripts) as well as prevent
    // treatment of known prerequisite types as such while still matching and
    // executing them (for example, plugin libraries).
    //
    // A rule with the "pass-through" semantics should treat the adhoc value
    // the same as true.
    //
    // To query this value in rule implementations use the include() helpers
    // from <libbuild2/prerequisites.hxx>.
    //
    // [string] prereq visibility
    //
    const variable* var_include;

    // The build.* namespace.
    //
    // .meta_operation
    //
    const variable* var_build_meta_operation;

    // Known meta-operation and operation tables.
    //
    build2::meta_operation_table meta_operation_table;
    build2::operation_table operation_table;

    // The old/new src_root remapping for subprojects.
    //
    dir_path old_src_root;
    dir_path new_src_root;

    // NULL if this context hasn't already locked the loaded_modules state.
    //
    const loaded_modules_lock* modules_lock;

    // Nested context for updating build system modules.
    //
    // Note that such a context itself should normally have modules_context
    // setup to point to itself (see import_module() for details).
    //
    context* module_context;
    optional<unique_ptr<context>> module_context_storage;

  public:
    // If module_context is absent, then automatic updating of build system
    // modules is disabled. If it is NULL, then the context will be created
    // lazily if and when necessary. Otherwise, it should be a properly setup
    // context (including, normally, a self-reference in modules_context).
    //
    explicit
    context (scheduler&,
             global_mutexes&,
             bool dry_run = false,
             bool keep_going = true,
             const strings& cmd_vars = {},
             optional<context*> module_context = nullptr,
             const loaded_modules_lock* inherited_mudules_lock = nullptr);

    // Set current meta-operation and operation.
    //
    void
    current_meta_operation (const meta_operation_info&);

    void
    current_operation (const operation_info& inner,
                       const operation_info* outer = nullptr,
                       bool diag_noise = true);

    context (context&&) = delete;
    context& operator= (context&&) = delete;

    context (const context&) = delete;
    context& operator= (const context&) = delete;

    ~context ();
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
  struct LIBBUILD2_SYMEXPORT phase_lock
  {
    explicit phase_lock (context&, run_phase);
    ~phase_lock ();

    phase_lock (phase_lock&&) = delete;
    phase_lock (const phase_lock&) = delete;

    phase_lock& operator= (phase_lock&&) = delete;
    phase_lock& operator= (const phase_lock&) = delete;

    context& ctx;
    phase_lock* prev; // From another context.
    run_phase phase;
  };

  // Assuming we have a lock on the current phase, temporarily release it
  // and reacquire on destruction.
  //
  struct LIBBUILD2_SYMEXPORT phase_unlock
  {
    phase_unlock (context&, bool unlock = true);
    ~phase_unlock () noexcept (false);

    phase_lock* l;
  };

  // Assuming we have a lock on the current phase, temporarily switch to a
  // new phase and switch back on destruction.
  //
  struct LIBBUILD2_SYMEXPORT phase_switch
  {
    explicit phase_switch (context&, run_phase);
    ~phase_switch () noexcept (false);

    run_phase old_phase, new_phase;
  };

  // Wait for a task count optionally and temporarily unlocking the phase.
  //
  struct wait_guard
  {
    ~wait_guard () noexcept (false);

    wait_guard (); // Empty.

    wait_guard (context&,
                atomic_count& task_count,
                bool phase = false);

    wait_guard (context&,
                size_t start_count,
                atomic_count& task_count,
                bool phase = false);

    void
    wait ();

    // Note: move-assignable to empty only.
    //
    wait_guard (wait_guard&&);
    wait_guard& operator= (wait_guard&&);

    wait_guard (const wait_guard&) = delete;
    wait_guard& operator= (const wait_guard&) = delete;

    context* ctx;
    size_t start_count;
    atomic_count* task_count;
    bool phase;
  };

  // Config module entry points.
  //
  LIBBUILD2_SYMEXPORT extern void (*config_save_variable) (
    scope&, const variable&, uint64_t flags);

  LIBBUILD2_SYMEXPORT extern const string& (*config_preprocess_create) (
    context&,
    values&,
    vector_view<opspec>&,
    bool lifted,
    const location&);
}

#include <libbuild2/context.ixx>

#endif // LIBBUILD2_CONTEXT_HXX
