// file      : build2/algorithm.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_ALGORITHM_HXX
#define BUILD2_ALGORITHM_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/action.hxx>
#include <build2/target.hxx>

namespace build2
{
  class scope;
  class prerequisite;
  class prerequisite_key;

  // The default prerequisite search implementation. It first calls the
  // prerequisite-type-specific search function. If that doesn't yeld
  // anything, it creates a new target.
  //
  const target&
  search (const target&, const prerequisite&);

  // As above but only search for an already existing target.
  //
  const target*
  search_existing (const prerequisite&);

  // As above but cache a target searched in a custom way.
  //
  const target&
  search_custom (const prerequisite&, const target&);

  // As above but specify the prerequisite to search as a key.
  //
  const target&
  search (const target&, const prerequisite_key&);

  const target*
  search_existing (const prerequisite_key&);

  // Uniform search interface for prerequisite/prerequisite_member.
  //
  inline const target&
  search (const target& t, const prerequisite_member& p) {return p.search (t);}

  // As above but override the target type. Useful for searching for
  // target group members where we need to search for a different
  // target type.
  //
  const target&
  search (const target&, const target_type&, const prerequisite_key&);

  // As above but specify the prerequisite to search as individual key
  // components. Scope can be NULL if the directory is absolute.
  //
  const target&
  search (const target&,
          const target_type& type,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext = nullptr,  // NULL means unspecified.
          const scope* = nullptr,       // NULL means dir is absolute.
          const optional<project_name>& proj = nullopt);

  const target*
  search_existing (const target_type& type,
                   const dir_path& dir,
                   const dir_path& out,
                   const string& name,
                   const string* ext = nullptr,
                   const scope* = nullptr,
                   const optional<project_name>& proj = nullopt);

  // As above but specify the target type as template argument.
  //
  template <typename T>
  const T&
  search (const target&,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const string* ext = nullptr,
          const scope* = nullptr);

  // Search for a target identified by the name. The semantics is "as if" we
  // first created a prerequisite based on this name in exactly the same way
  // as the parser would and then searched based on this prerequisite.
  //
  const target&
  search (const target&, name, const scope&);

  // Unlike the above version, this one can be called during the execute
  // phase. Return NULL for unknown target types.
  //
  const target*
  search_existing (const name&,
                   const scope&,
                   const dir_path& out = dir_path ());

  // Target match lock: a non-const target reference and the target::offset_*
  // state that has already been "achieved". Note that target::task_count
  // itself is set to busy for the duration or the lock. While at it we also
  // maintain a stack of active locks in the current dependency chain (used to
  // detect dependency cycles).
  //
  struct target_lock
  {
    using action_type = build2::action;
    using target_type = build2::target;

    action_type  action;
    target_type* target = nullptr;
    size_t       offset = 0;

    explicit operator bool () const {return target != nullptr;}

    void
    unlock ();

    // Movable-only type with move-assignment only to NULL lock.
    //
    target_lock () = default;
    target_lock (target_lock&&);
    target_lock& operator= (target_lock&&);

    target_lock (const target_lock&) = delete;
    target_lock& operator= (const target_lock&) = delete;

    // Implementation details.
    //
    ~target_lock ();
    target_lock (action_type, target_type*, size_t);

    struct data
    {
      action_type action;
      target_type* target;
      size_t offset;
    };

    data
    release ();

    static
#ifdef __cpp_thread_local
    thread_local
#else
    __thread
#endif
    const target_lock* stack; // Tip of the stack.
    const target_lock* prev;

    struct stack_guard
    {
      explicit stack_guard (const target_lock* s): s_ (stack) {stack = s;}
      ~stack_guard () {stack = s_;}
      const target_lock* s_;
    };
  };

  // If this target is already locked in this dependency chain, then return
  // the corresponding lock. Return NULL otherwise (so can be used a boolean
  // predicate).
  //
  const target_lock*
  dependency_cycle (action, const target&);

  // If the target is already applied (for this action ) or executed, then no
  // lock is acquired. Otherwise, the target must not yet be matched for this
  // action.
  //
  // @@ MT fuzzy: what if it is already in the desired state, why assert?
  //    Currently we only use it with match_recipe().
  //
  target_lock
  lock (action, const target&);

  // Add an ad hoc member to the end of the chain assuming that an already
  // existing member of this target type is the same. Return the locked member
  // target.
  //
  target_lock
  add_adhoc_member (action,
                    target&,
                    const target_type&,
                    const dir_path& dir,
                    const dir_path& out,
                    const string& name);

  // If the suffix is specified, it is added (as an extension) to the member's
  // target name.
  //
  target_lock
  add_adhoc_member (action,
                    target&,
                    const target_type&,
                    const char* suffix = nullptr);

  template <typename T>
  inline target_lock
  add_adhoc_member (action a, target& t, const char* s = nullptr)
  {
    return add_adhoc_member (a, t, T::static_type, s);
  }

  // Find an ad hoc member of the specified target type returning NULL if not
  // found.
  //
  const target*
  find_adhoc_member (const target&, const target_type&);

  template <typename T>
  inline const T*
  find_adhoc_member (const target& t)
  {
    return static_cast<const T*> (find_adhoc_member (t, T::static_type));
  }

  // Match and apply a rule to the action/target with ambiguity detection.
  // Increment the target's dependents count, which means that you should call
  // this function with the intent to also call execute(). Return the target
  // state translating target_state::failed to the failed exception unless
  // instructed otherwise.
  //
  // The try_match() version doesn't issue diagnostics if there is no rule
  // match (but fails as match() for all other errors, like rule ambiguity,
  // inability to apply, etc). The first half of the result indicated whether
  // there was a rule match.
  //
  // The unmatch argument allows optimizations that avoid calling execute().
  // If it is unmatch::unchanged then only unmatch the target if it is known
  // to be unchanged after match. If it is unmatch::safe, then unmatch the
  // target if it is safe (this includes unchanged or if we know that someone
  // else will execute this target). Return true if unmatch succeeded. Always
  // throw if failed.
  //
  enum class unmatch {none, unchanged, safe};

  target_state
  match (action, const target&, bool fail = true);

  pair<bool, target_state>
  try_match (action, const target&, bool fail = true);

  bool
  match (action, const target&, unmatch);

  // Start asynchronous match. Return target_state::postponed if the
  // asynchrounous operation has been started and target_state::busy if the
  // target has already been busy. Regardless of the result, match() must be
  // called in order to complete the operation (except target_state::failed).
  //
  // If fail is false, then return target_state::failed if the target match
  // failed. Otherwise, throw the failed exception if keep_going is false and
  // return target_state::failed otherwise.
  //
  target_state
  match_async (action, const target&,
               size_t start_count, atomic_count& task_count,
               bool fail = true);

  // Match by specifying the recipe directly. The target must be locked.
  //
  void
  match_recipe (target_lock&, recipe);

  // Match a "delegate rule" from withing another rules' apply() function
  // avoiding recursive matches (thus the third argument). Unless try_match is
  // true, fail if no rule is found. Otherwise return empty recipe. Note that
  // unlike match(), this function does not increment the dependents count and
  // the two rules must coordinate who is using the target's data pad and/or
  // prerequisite_targets. See also the companion execute_delegate().
  //
  recipe
  match_delegate (action, target&, const rule&, bool try_match = false);

  // Match a rule for the inner operation from withing the outer rule's
  // apply() function. See also the companion execute_inner().
  //
  target_state
  match_inner (action, const target&);

  bool
  match_inner (action, const target&, unmatch);

  // The standard prerequisite search and match implementations. They call
  // search() (unless a custom is provided) and then match() (unless custom
  // returned NULL) for each prerequisite in a loop omitting out of project
  // prerequisites for the clean operation. If this target is a member of a
  // group, then first do this to the group's prerequisites.
  //
  using match_search =function<
    prerequisite_target (action,
                         const target&,
                         const prerequisite&,
                         include_type)>;

  void
  match_prerequisites (action, target&, const match_search& = nullptr);

  // As above but go into group members.
  //
  // Note that if we cleaning, this function doesn't go into group members, as
  // an optimization (the group should clean everything up).
  //
  using match_search_member = function<
    prerequisite_target (action,
                         const target&,
                         const prerequisite_member&,
                         include_type)>;

  void
  match_prerequisite_members (action, target&,
                              const match_search_member& = nullptr);

  // As above but omit prerequisites that are not in the specified scope.
  //
  void
  match_prerequisites (action, target&, const scope&);

  void
  match_prerequisite_members (action, target&, const scope&);

  // Match (already searched) members of a group or similar prerequisite-like
  // dependencies. Similar in semantics to match_prerequisites(). Any marked
  // target pointers are skipped.
  //
  // T can only be const target* or prerequisite_target.
  //
  template <typename T>
  void
  match_members (action, target&, T const*, size_t);

  template <size_t N>
  inline void
  match_members (action a, target& t, const target* (&ts)[N])
  {
    match_members (a, t, ts, N);
  }

  inline void
  match_members (action a,
                 target& t,
                 prerequisite_targets& ts,
                 size_t start = 0)
  {
    match_members (a, t, ts.data () + start, ts.size () - start);
  }

  // Unless already known, match, and, if necessary, execute the group in
  // order to resolve its members list. Note that even after that the member's
  // list might still not be available (e.g., if some wildcard/ fallback rule
  // matched).
  //
  // If the action is for an outer operation, then it is changed to inner
  // which means the members are always resolved by the inner (e.g., update)
  // rule. This feels right since this is the rule that will normally do the
  // work (e.g., update) and therefore knows what it will produce (and if we
  // don't do this, then the group resolution will be racy since we will use
  // two different task_count instances for synchronization).
  //
  group_view
  resolve_members (action, const target&);

  // Unless already known, match the target in order to resolve its group.
  //
  // Unlike the member case, a rule can only decide whether a target is a
  // member of the group in its match() since otherwise it (presumably) should
  // not match (and some other rule may).
  //
  // If the action is for an outer operation, then it is changed to inner, the
  // same as for members.
  //
  const target*
  resolve_group (action, const target&);

  // Inject dependency on the target's directory fsdir{}, unless it is in the
  // src tree or is outside of any project (say, for example, an installation
  // directory). If the parent argument is true, then inject the parent
  // directory of a target that is itself a directory (name is empty). Return
  // the injected target or NULL. Normally this function is called from the
  // rule's apply() function.
  //
  // As an extension, this function will also search for an existing fsdir{}
  // prerequisite for the directory and if one exists, return that (even if
  // the target is in src tree). This can be used, for example, to place
  // output into an otherwise non-existent directory.
  //
  const fsdir*
  inject_fsdir (action, target&, bool parent = true);

  // Execute the action on target, assuming a rule has been matched and the
  // recipe for this action has been set. This is the synchrounous executor
  // implementation (but may still return target_state::busy if the target
  // is already being executed). Decrements the dependents count.
  //
  // Note: does not translate target_state::failed to the failed exception.
  //
  target_state
  execute (action, const target&);

  // As above but wait for completion if the target is busy and translate
  // target_state::failed to the failed exception.
  //
  target_state
  execute_wait (action, const target&);

  // As above but start asynchronous execution. Return target_state::unknown
  // if the asynchrounous execution has been started and target_state::busy if
  // the target has already been busy.
  //
  // If fail is false, then return target_state::failed if the target match
  // failed. Otherwise, throw the failed exception if keep_going is false and
  // return target_state::failed otherwise.
  //
  target_state
  execute_async (action, const target&,
                 size_t start_count, atomic_count& task_count,
                 bool fail = true);

  // Execute the recipe obtained with match_delegate(). Note that the target's
  // state is neither checked nor updated by this function. In other words,
  // the appropriate usage is to call this function from another recipe and to
  // factor the obtained state into the one returned.
  //
  target_state
  execute_delegate (const recipe&, action, const target&);

  // Execute the inner operation matched with match_inner(). Note that the
  // returned target state is for the inner operation. The appropriate usage
  // is to call this function from the outer operation's recipe and to factor
  // the obtained state into the one returned (similar to how we do it for
  // prerequisites).
  //
  // Note: waits for the completion if the target is busy and translates
  // target_state::failed to the failed exception.
  //
  target_state
  execute_inner (action, const target&);

  // A special version of the above that should be used for "direct" and "now"
  // execution, that is, side-stepping the normal target-prerequisite
  // relationship (so no dependents count is decremented) and execution order
  // (so this function never returns the postponed target state).
  //
  // Note: waits for the completion if the target is busy and translates
  // target_state::failed to the failed exception.
  //
  target_state
  execute_direct (action, const target&);

  // The default prerequisite execute implementation. Call execute_async() on
  // each non-ignored (non-NULL) prerequisite target in a loop and then wait
  // for their completion. Return target_state::changed if any of them were
  // changed and target_state::unchanged otherwise. If a prerequisite's
  // execution is postponed (and thus its state cannot be queried MT-safely)
  // of if the prerequisite is marked as ad hoc, then set its pointer in
  // prerequisite_targets to NULL. If count is not 0, then only the first
  // count prerequisites are executed beginning from start.
  //
  target_state
  straight_execute_prerequisites (action, const target&,
                                  size_t count = 0, size_t start = 0);

  // As above but iterates over the prerequisites in reverse.
  //
  target_state
  reverse_execute_prerequisites (action, const target&, size_t count = 0);

  // Call straight or reverse depending on the current mode.
  //
  target_state
  execute_prerequisites (action, const target&, size_t count = 0);

  // As above but execute prerequisites for the inner action (that have
  // been matched with match_inner()).
  //
  target_state
  straight_execute_prerequisites_inner (action, const target&,
                                        size_t count = 0, size_t start = 0);

  target_state
  reverse_execute_prerequisites_inner (action, const target&, size_t count = 0);

  target_state
  execute_prerequisites_inner (action, const target&, size_t count = 0);

  // A version of the above that also determines whether the action needs to
  // be executed on the target based on the passed timestamp and filter.
  //
  // The filter is passed each prerequisite target and is expected to signal
  // which ones should be used for timestamp comparison. If the filter is
  // NULL, then all the prerequisites are used. If count is not 0, then only
  // the first count prerequisites are executed.
  //
  // Note that the return value is an optional target state. If the target
  // needs updating, then the value is absent. Otherwise it is the state that
  // should be returned. This is used to handle the situation where some
  // prerequisites were updated but no update of the target is necessary. In
  // this case we still signal that the target was (conceptually, but not
  // physically) changed. This is important both to propagate the fact that
  // some work has been done and to also allow our dependents to detect this
  // case if they are up to something tricky (like recursively linking liba{}
  // prerequisites).
  //
  // Note that because we use mtime, this function should normally only be
  // used in the perform_update action (which is straight).
  //
  using execute_filter = function<bool (const target&, size_t pos)>;

  optional<target_state>
  execute_prerequisites (action, const target&,
                         const timestamp&,
                         const execute_filter& = nullptr,
                         size_t count = 0);

  // Another version of the above that does two extra things for the caller:
  // it determines whether the action needs to be executed on the target based
  // on the passed timestamp and finds a prerequisite of the specified type
  // (e.g., a source file). If there are multiple prerequisites of this type,
  // then the first is returned (this can become important if additional
  // prerequisites of the same type may get injected).
  //
  template <typename T>
  pair<optional<target_state>, const T&>
  execute_prerequisites (action, const target&,
                         const timestamp&,
                         const execute_filter& = nullptr,
                         size_t count = 0);

  pair<optional<target_state>, const target&>
  execute_prerequisites (const target_type&,
                         action, const target&,
                         const timestamp&,
                         const execute_filter& = nullptr,
                         size_t count = 0);

  template <typename T>
  pair<optional<target_state>, const T&>
  execute_prerequisites (const target_type&,
                         action, const target&,
                         const timestamp&,
                         const execute_filter& = nullptr,
                         size_t count = 0);

  // Execute members of a group or similar prerequisite-like dependencies.
  // Similar in semantics to execute_prerequisites().
  //
  // T can only be const target* or prerequisite_target. If it is the latter,
  // the ad hoc semantics described in execute_prerequsites() is in effect.
  //
  template <typename T>
  target_state
  straight_execute_members (action, atomic_count&, T[], size_t, size_t);

  template <typename T>
  target_state
  reverse_execute_members (action, atomic_count&, T[], size_t, size_t);

  template <typename T>
  inline target_state
  straight_execute_members (action a, const target& t,
                            T ts[], size_t c, size_t s)
  {
    return straight_execute_members (a, t[a].task_count, ts, c, s);
  }

  template <typename T>
  inline target_state
  reverse_execute_members (action a, const target& t,
                           T ts[], size_t c, size_t s)
  {
    return reverse_execute_members (a, t[a].task_count, ts, c, s);
  }

  // Call straight or reverse depending on the current mode.
  //
  target_state
  execute_members (action, const target&, const target*[], size_t);

  template <size_t N>
  inline target_state
  straight_execute_members (action a, const target& t, const target* (&ts)[N])
  {
    return straight_execute_members (a, t, ts, N, 0);
  }

  template <size_t N>
  inline target_state
  reverse_execute_members (action a, const target& t, const target* (&ts)[N])
  {
    return reverse_execute_members (a, t, ts, N, N);
  }

  template <size_t N>
  inline target_state
  execute_members (action a, const target& t, const target* (&ts)[N])
  {
    return execute_members (a, t, ts, N);
  }

  // Return noop_recipe instead of using this function directly.
  //
  target_state
  noop_action (action, const target&);

  // Default action implementation which forwards to the prerequisites.
  // Use default_recipe instead of using this function directly.
  //
  target_state
  default_action (action, const target&);

  // Standard perform(clean) action implementation for the file target
  // (or derived).
  //
  target_state
  perform_clean (action, const target&);

  // As above, but also removes the auxiliary dependency database (.d file).
  //
  target_state
  perform_clean_depdb (action, const target&);

  // As above but clean the target group. The group should be an mtime_target
  // and members should be files.
  //
  target_state
  perform_clean_group (action, const target&);

  // As above but clean both the target group and depdb. The depdb file path
  // is derived from the first member file path.
  //
  target_state
  perform_clean_group_depdb (action, const target&);

  // Helper for custom perform(clean) implementations that cleans extra files
  // and directories (recursively) specified as a list of either absolute
  // paths or "path derivation directives". The directive string can be NULL,
  // or empty in which case it is ignored. If the last character in a
  // directive is '/', then the resulting path is treated as a directory
  // rather than a file.  The directive can start with zero or more '-'
  // characters which indicate the number of extensions that should be
  // stripped before the new extension (if any) is added (so if you want to
  // strip the extension, specify just "-"). For example:
  //
  // clean_extra (a, t, {".d", ".dlls/", "-.dll"});
  //
  // The extra files/directories are removed first in the specified order
  // followed by the ad hoc group member, then target itself, and, finally,
  // the prerequisites in the reverse order.
  //
  // You can also clean extra files derived from ad hoc group members.
  //
  // Note that if the target path is empty then it is assumed "unreal" and
  // is not cleaned (but its prerequisites/members still are).
  //
  target_state
  clean_extra (action, const file&,
               initializer_list<initializer_list<const char*>> extra);

  inline target_state
  clean_extra (action a, const file& f, initializer_list<const char*> extra)
  {
    return clean_extra (a, f, {extra});
  }

  // Update/clean a backlink issuing appropriate diagnostics at appropriate
  // levels depending on the overload and the changed argument.
  //
  enum class backlink_mode
  {
    link,      // Make a symbolic link if possible, hard otherwise.
    symbolic,  // Make a symbolic link.
    hard,      // Make a hard link.
    copy,      // Make a copy.
    overwrite  // Copy over but don't remove on clean (committed gen code).
  };

  void
  update_backlink (const file& target,
                   const path& link,
                   bool changed,
                   backlink_mode = backlink_mode::link);

  void
  update_backlink (const path& target,
                   const path& link,
                   bool changed,
                   backlink_mode = backlink_mode::link);

  void
  update_backlink (const path& target,
                   const path& link,
                   backlink_mode = backlink_mode::link);

  void
  clean_backlink (const path& link,
                  uint16_t verbosity,
                  backlink_mode = backlink_mode::link);
}

#include <build2/algorithm.ixx>

#endif // BUILD2_ALGORITHM_HXX
