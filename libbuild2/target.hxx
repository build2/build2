// file      : libbuild2/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TARGET_HXX
#define LIBBUILD2_TARGET_HXX

#include <iterator>     // tags, etc.
#include <type_traits>  // aligned_storage
#include <unordered_map>

#include <libbutl/multi-index.mxx> // map_iterator_adapter

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/action.hxx>
#include <libbuild2/recipe.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/target-key.hxx>
#include <libbuild2/target-type.hxx>
#include <libbuild2/target-state.hxx>
#include <libbuild2/prerequisite.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // From <libbuild2/algorithm.hxx>.
  //
  LIBBUILD2_SYMEXPORT const target&
  search (const target&, const prerequisite&);

  LIBBUILD2_SYMEXPORT const target*
  search_existing (const prerequisite&);

  // Prerequisite inclusion/exclusion (see include() function below).
  //
  class include_type
  {
  public:
    enum value {excluded, adhoc, normal};

    include_type (value v): v_ (v) {}
    include_type (bool  v): v_ (v ? normal : excluded) {}

    operator         value () const {return v_;}
    explicit operator bool () const {return v_ != excluded;}

  private:
    value v_;
  };

  // A view of target group members.
  //
  struct group_view
  {
    const target* const* members; // NULL means not yet known.
    size_t count;
  };

  // List of prerequisites resolved to targets. Unless additional storage is
  // needed, it can be used as just vector<const target*> (which is what we
  // used to have initially).
  //
  struct prerequisite_target
  {
    using target_type = build2::target;

    prerequisite_target (const target_type* t, bool a = false, uintptr_t d = 0)
        : target (t), adhoc (a), data (d) {}

    prerequisite_target (const target_type* t, include_type a, uintptr_t d = 0)
        : prerequisite_target (t, a == include_type::adhoc, d) {}

    operator const target_type*&  ()       {return target;}
    operator const target_type*   () const {return target;}
    const target_type* operator-> () const {return target;}

    const target_type* target;
    bool               adhoc;  // True if include=adhoc.
    uintptr_t          data;
  };
  using prerequisite_targets = vector<prerequisite_target>;

  // A rule match is an element of hint_rule_map.
  //
  using rule_match = pair<const string, reference_wrapper<const rule>>;

  // Target.
  //
  class LIBBUILD2_SYMEXPORT target
  {
  public:
    // Context this target belongs to.
    //
    context& ctx;

    // For targets that are in the src tree of a project we also keep the
    // corresponding out directory. As a result we may end up with multiple
    // targets for the same file if we are building multiple configurations of
    // the same project at once. We do it this way because, in a sense, a
    // target's out directory is its "configuration" (in terms of variables).
    // As an example, consider installing the same README file (src) but for
    // two different project configurations at once. Which installation
    // directory should we use? The answer depends on which configuration you
    // ask.
    //
    // Empty out directory indicates this target is in the out tree (including
    // when src == out). We also treat out of project targets as being in the
    // out tree.
    //
    const dir_path    dir;  // Absolute and normalized.
    const dir_path    out;  // Empty or absolute and normalized.
    const string      name;
    optional<string>* ext_; // Reference to value in target_key.

    const string* ext () const; // Return NULL if not specified.
    const string& ext (string);

    const dir_path&
    out_dir () const {return out.empty () ? dir : out;}

    // A target that is not (yet) entered as part of a real dependency
    // declaration (for example, that is entered as part of a target-specific
    // variable assignment, dependency extraction, etc) is called implied.
    //
    // The implied flag should only be cleared during the load phase via the
    // MT-safe target_set::insert().
    //
    bool implied;

    // Target group to which this target belongs, if any. Note that we assume
    // that the group and all its members are in the same scope (for example,
    // in variable lookup). We also don't support nested groups (with an
    // exception for ad hoc groups; see below).
    //
    // The semantics of the interaction between the group and its members and
    // what it means to, say, update the group, is unspecified and is
    // determined by the group's type. In particular, a group can be created
    // out of member types that have no idea they are part of this group
    // (e.g., cli.cxx{}).
    //
    // Normally, however, there are two kinds of groups: "all" and "choice".
    // In a choice-group, normally one of the members is selected when the
    // group is mentioned as a prerequisite with, perhaps, an exception for
    // special rules, like aliases, where it makes more sense to treat such
    // group prerequisites as a whole. In this case we say that the rule
    // "semantically recognizes" the group and picks some of its members.
    //
    // Updating a choice-group as a whole can mean updating some subset of its
    // members (e.g., lib{}). Or the group may not support this at all (e.g.,
    // obj{}).
    //
    // In an all-group, when a group is updated, normally all its members are
    // updates (and usually with a single command), though there could be some
    // members that are omitted, depending on the configuration (e.g., an
    // inline file not/being generated). When an all-group is mentioned as a
    // prerequisite, the rule is usually interested in the individual members
    // rather than the whole group. For example, a C++ compile rule would like
    // to "see" the ?xx{} members when it gets a cli.cxx{} group.
    //
    // Which brings us to the group iteration mode. The target type contains a
    // member called see_through that indicates whether the default iteration
    // mode for the group should be "see through"; that is, whether we see the
    // members or the group itself. For the iteration support itself, see the
    // *_prerequisite_members() machinery below.
    //
    // In an all-group we usually want the state (and timestamp; see mtime())
    // for members to come from the group. This is achieved with the special
    // target_state::group state. You would normally also use the group_recipe
    // for group members.
    //
    // Note that the group-member link-up can happen anywhere between the
    // member creation and rule matching so reading the group before the
    // member has been matched can be racy.
    //
    const target* group = nullptr;

    // What has been described above is a "explicit" group. That is, there is
    // a dedicated target type that explicitly serves as a group and there is
    // an explicit mechanism for discovering the group's members.
    //
    // However, sometimes, we may want to create a group on the fly out of a
    // normal target type. For example, we have the libs{} target type. But
    // on Windows a shared library consist of (at least) two files: the import
    // library and the DLL itself. So we somehow need to be able to capture
    // that. One approach would be to imply the presence of the second file.
    // However, that means that a lot of generic rules (e.g., clean, install,
    // etc) will need to know about this special semantics on Windows. Also,
    // there would be no convenient way to customize things like extensions,
    // etc (for which we use target-specific variables). In other words, it
    // would be much easier and more consistent to make these extra files
    // proper targets.
    //
    // So to support this requirement we have "ad hoc" groups. The idea is
    // that any target can be turned either by a user's declaration in a
    // buildfile or by the rule that matches it into an ad hoc group by
    // chaining several targets together.
    //
    // Ad hoc groups have a more restricted semantics compared to the normal
    // groups. In particular:
    //
    // - The ad hoc group itself is in a sense its first/primary target.
    //
    // - Group member's recipes, if set, should be group_recipe. Normally, a
    //   rule-managed member isn't matched by the rule since all that's
    //   usually needed is to derive its path.
    //
    // - Unless declared, members are discovered lazily, they are only known
    //   after the group's rule's apply() call.
    //
    // - Only declared members can be used as prerequisites but all can be
    //   used as targets (e.g., to set variables, etc).
    //
    // - Members don't have prerequisites.
    //
    // - Ad hoc group cannot have sub-groups (of any kind) though an ad hoc
    //   group can be a sub-group of an explicit group.
    //
    // - Member variable lookup skips the ad hoc group (since the group is the
    //   first member, this is normally what we want).
    //
    // Note that ad hoc groups can be part of explicit groups. In a sense, we
    // have a two-level grouping: an explicit group with its members each of
    // which can be an ad hoc group. For example, lib{} contains libs{} which
    // may have an import stub as its ad hoc member.
    //
    // Use add_adhoc_member(), find_adhoc_member() from algorithms to manage
    // ad hoc members.
    //
    const_ptr<target> member = nullptr;

    bool
    adhoc_group () const
    {
      // An ad hoc group can be a member of a normal group.
      //
      return member != nullptr &&
        (group == nullptr || group->member == nullptr);
    }

    bool
    adhoc_member () const
    {
      return group != nullptr && group->member != nullptr;
    }

  public:
    // Normally you should not call this function directly and rather use
    // resolve_members() from <libbuild2/algorithm.hxx>.
    //
    virtual group_view
    group_members (action) const;

    // Note that the returned key "tracks" the target (except for the
    // extension).
    //
    target_key
    key () const;

    names
    as_name () const;

    // Scoping.
    //
   public:
    // Most qualified scope that contains this target.
    //
    const scope&
    base_scope () const;

    // Root scope of a project that contains this target. Note that
    // a target can be out of any (known) project root in which case
    // this function asserts. If you need to detect this situation,
    // then use base_scope().root_scope() expression instead.
    //
    const scope&
    root_scope () const;

    // Root scope of a strong amalgamation that contains this target.
    // The same notes as to root_scope() apply.
    //
    const scope&
    strong_scope () const {return *root_scope ().strong_scope ();}

    // Root scope of the outermost amalgamation that contains this target.
    // The same notes as to root_scope() apply.
    //
    const scope&
    weak_scope () const {return *root_scope ().weak_scope ();}

    bool
    in (const scope& s) const
    {
      return out_dir ().sub (s.out_path ());
    }

    // Prerequisites.
    //
    // We use an atomic-empty semantics that allows one to "swap in" a set of
    // prerequisites if none were specified. This is used to implement
    // "synthesized" dependencies.
    //
  public:
    using prerequisites_type = build2::prerequisites;

    const prerequisites_type&
    prerequisites () const;

    // Swap-in a list of prerequisites. Return false if unsuccessful (i.e.,
    // someone beat us to it). Note that it can be called on const target.
    //
    bool
    prerequisites (prerequisites_type&&) const;

    // Check if there are any prerequisites. Note that the group version may
    // be racy (see target::group).
    //
    bool
    has_prerequisites () const;

    bool
    has_group_prerequisites () const;

  private:
    friend class parser;

    // Note that the state is also used to synchronize the prerequisites
    // value so we use the release-acquire ordering.
    //
    // 0 - absent
    // 1 - being set
    // 2 - present
    //
    atomic<uint8_t> prerequisites_state_ {0};
    prerequisites_type prerequisites_;

    static const prerequisites_type empty_prerequisites_;

    // Target-specific variables.
    //
    // See also rule-specific variables below.
    //
  public:
    variable_map vars;

    // Lookup, including in groups to which this target belongs and then in
    // outer scopes (including target type/pattern-specific variables). If you
    // only want to lookup in this target, do it on the variable map directly
    // (and note that there will be no overrides).
    //
    using lookup_type = build2::lookup;

    lookup_type
    operator[] (const variable& var) const
    {
      return lookup (var).first;
    }

    lookup_type
    operator[] (const variable* var) const // For cached variables.
    {
      assert (var != nullptr);
      return operator[] (*var);
    }

    lookup_type
    operator[] (const string& name) const
    {
      const variable* var (ctx.var_pool.find (name));
      return var != nullptr ? operator[] (*var) : lookup_type ();
    }

    // As above but also return the depth at which the value is found. The
    // depth is calculated by adding 1 for each test performed. So a value
    // that is from the target will have depth 1. That from the group -- 2.
    // From the innermost scope's target type/patter-specific variables --
    // 3. From the innermost scope's variables -- 4. And so on.  The idea is
    // that given two lookups from the same target, we can say which one came
    // earlier. If no value is found, then the depth is set to ~0.
    //
    pair<lookup_type, size_t>
    lookup (const variable& var) const
    {
      auto p (lookup_original (var));
      return var.overrides == nullptr
        ? p
        : base_scope ().lookup_override (var, move (p), true);
    }

    // If target_only is true, then only look in target and its target group
    // without continuing in scopes.
    //
    pair<lookup_type, size_t>
    lookup_original (const variable&, bool target_only = false) const;

    // Return a value suitable for assignment. See scope for details.
    //
    value&
    assign (const variable& var) {return vars.assign (var);}

    value&
    assign (const variable* var) {return vars.assign (var);} // For cached.

    // Return a value suitable for appending. See scope for details.
    //
    value&
    append (const variable&);

    // Target operation state.
    //
  public:
    // Atomic task count that is used during match and execution to track the
    // target's "meta-state" as well as the number of its sub-tasks (e.g.,
    // busy+1, busy+2, and so on, for instance, number of prerequisites
    // being matched or executed).
    //
    // For each operation in a meta-operation batch (current_on) we have a
    // "band" of counts, [touched, executed], that represent the target
    // meta-state. Once the next operation is started, this band "moves" thus
    // automatically resetting the target to "not yet touched" state for this
    // operation.
    //
    // The target is said to be synchronized (in this thread) if we have
    // either observed the task count to reach applied or executed or we have
    // successfully changed it (via compare_exchange) to locked or busy. If
    // the target is synchronized, then we can access and modify (second case)
    // its state etc.
    //
    // NOTE: see also the corresponding count_*() fuctions in context (must be
    //       kept in sync).
    //
    static const size_t offset_touched  = 1; // Target has been locked.
    static const size_t offset_tried    = 2; // Rule match has been tried.
    static const size_t offset_matched  = 3; // Rule has been matched.
    static const size_t offset_applied  = 4; // Rule has been applied.
    static const size_t offset_executed = 5; // Recipe has been executed.
    static const size_t offset_busy     = 6; // Match/execute in progress.

    // Inner/outer operation state. See <libbuild2/action.hxx> for details.
    //
    class LIBBUILD2_SYMEXPORT opstate
    {
    public:
      mutable atomic_count task_count {0}; // Start offset_touched - 1.

      // Number of direct targets that depend on this target in the current
      // operation. It is incremented during match and then decremented during
      // execution, before running the recipe. As a result, the recipe can
      // detect the last chance (i.e., last dependent) to execute the command
      // (see also the first/last execution modes in <libbuild2/operation.hxx>).
      //
      mutable atomic_count dependents {0};

      // Matched rule (pointer to hint_rule_map element). Note that in case of
      // a direct recipe assignment we may not have a rule (NULL).
      //
      const rule_match* rule;

      // Applied recipe.
      //
      build2::recipe recipe;

      // Target state for this operation. Note that it is undetermined until
      // a rule is matched and recipe applied (see set_recipe()).
      //
      target_state state;

      // Rule-specific variables.
      //
      // The rule (for this action) has to be matched before these variables
      // can be accessed and only the rule being matched can modify them (so
      // no iffy modifications of the group's variables by member's rules).
      //
      // They are also automatically cleared before another rule is matched,
      // similar to the data pad. In other words, rule-specific variables are
      // only valid for this match-execute phase.
      //
      variable_map vars;

      // Lookup, continuing in the target-specific variables, etc. Note that
      // the group's rule-specific variables are not included. If you only
      // want to lookup in this target, do it on the variable map directly
      // (and note that there will be no overrides).
      //
      using lookup_type = build2::lookup;

      lookup_type
      operator[] (const variable& var) const
      {
        return lookup (var).first;
      }

      lookup_type
      operator[] (const variable* var) const // For cached variables.
      {
        assert (var != nullptr);
        return operator[] (*var);
      }

      lookup_type
      operator[] (const string& name) const
      {
        const variable* var (target_->ctx.var_pool.find (name));
        return var != nullptr ? operator[] (*var) : lookup_type ();
      }

      // As above but also return the depth at which the value is found. The
      // depth is calculated by adding 1 for each test performed. So a value
      // that is from the rule will have depth 1. That from the target - 2,
      // and so on, similar to target-specific variables.
      //
      pair<lookup_type, size_t>
      lookup (const variable& var) const
      {
        auto p (lookup_original (var));
        return var.overrides == nullptr
          ? p
          : target_->base_scope ().lookup_override (var, move (p), true, true);
      }

      // If target_only is true, then only look in target and its target group
      // without continuing in scopes.
      //
      pair<lookup_type, size_t>
      lookup_original (const variable&, bool target_only = false) const;

      // Return a value suitable for assignment. See target for details.
      //
      value&
      assign (const variable& var) {return vars.assign (var);}

      value&
      assign (const variable* var) {return vars.assign (var);} // For cached.

    public:
      explicit
      opstate (context& c): vars (c, false /* global */) {}

    private:
      friend class target_set;

      const target* target_ = nullptr; // Back-pointer, set by target_set.
    };

    action_state<opstate> state;

    opstate&       operator[] (action a)       {return state[a];}
    const opstate& operator[] (action a) const {return state[a];}

    // Return true if the target has been matched for the specified action.
    // This function can only be called during execution.
    //
    bool
    matched (action) const;

    // This function can only be called during match if we have observed
    // (synchronization-wise) that this target has been matched (i.e., the
    // rule has been applied) for this action.
    //
    target_state
    matched_state (action, bool fail = true) const;

    // See try_match().
    //
    pair<bool, target_state>
    try_matched_state (action, bool fail = true) const;

    // After the target has been matched and synchronized, check if the target
    // is known to be unchanged. Used for optimizations during search & match.
    //
    bool
    unchanged (action) const;

    // This function can only be called during execution if we have observed
    // (synchronization-wise) that this target has been executed.
    //
    target_state
    executed_state (action, bool fail = true) const;

  protected:
    // Version that should be used during match after the target has been
    // matched for this action.
    //
    // Indicate whether there is a rule match with the first half of the
    // result (see try_match()).
    //
    pair<bool, target_state>
    matched_state_impl (action) const;

    // Return fail-untranslated (but group-translated) state assuming the
    // target is executed and synchronized.
    //
    target_state
    executed_state_impl (action) const;

    // Return true if the state comes from the group. Target must be at least
    // matched.
    //
    bool
    group_state (action) const;

  public:
    // Targets to which prerequisites resolve for this action. Note that
    // unlike prerequisite::target, these can be resolved to group members.
    // NULL means the target should be skipped (or the rule may simply not add
    // such a target to the list).
    //
    // Note also that it is possible the target can vary from action to
    // action, just like recipes. We don't need to keep track of the action
    // here since the targets will be updated if the recipe is updated,
    // normally as part of rule::apply().
    //
    // Note that the recipe may modify this list.
    //
    mutable action_state<build2::prerequisite_targets> prerequisite_targets;

    // Auxilary data storage.
    //
    // A rule that matches (i.e., returns true from its match() function) may
    // use this pad to pass data between its match and apply functions as well
    // as the recipe. After the recipe is executed, the data is destroyed by
    // calling data_dtor (if not NULL). The rule should static assert that the
    // size of the pad is sufficient for its needs.
    //
    // Note also that normally at least 2 extra pointers may be stored without
    // a dynamic allocation in the returned recipe (small object optimization
    // in std::function). So if you need to pass data only between apply() and
    // the recipe, then this might be a more convenient way.
    //
    // Note also that a rule that delegates to another rule may not be able to
    // use this mechanism fully since the delegated-to rule may also need the
    // data pad.
    //
    // Currenly the data is not destroyed until the next match.
    //
    // Note that the recipe may modify the data. Currently reserved for the
    // inner part of the action.
    //
    static constexpr size_t data_size = sizeof (string) * 16;
    mutable std::aligned_storage<data_size>::type data_pad;

    mutable void (*data_dtor) (void*)                = nullptr;

    template <typename R,
              typename T = typename std::remove_cv<
                typename std::remove_reference<R>::type>::type>
    typename std::enable_if<std::is_trivially_destructible<T>::value,T&>::type
    data (R&& d) const
    {
      assert (sizeof (T) <= data_size && data_dtor == nullptr);
      return *new (&data_pad) T (forward<R> (d));
    }

    template <typename R,
              typename T = typename std::remove_cv<
                typename std::remove_reference<R>::type>::type>
    typename std::enable_if<!std::is_trivially_destructible<T>::value,T&>::type
    data (R&& d) const
    {
      assert (sizeof (T) <= data_size && data_dtor == nullptr);
      T& r (*new (&data_pad) T (forward<R> (d)));
      data_dtor = [] (void* p) {static_cast<T*> (p)->~T ();};
      return r;
    }

    template <typename T>
    T&
    data () const {return *reinterpret_cast<T*> (&data_pad);}

    void
    clear_data () const
    {
      if (data_dtor != nullptr)
      {
        data_dtor (&data_pad);
        data_dtor = nullptr;
      }
    }

    // Target type info and casting.
    //
  public:
    const target*
    is_a (const target_type& tt) const {
      return type ().is_a (tt) ? this : nullptr;}

    template <typename T>
    T*
    is_a () {return dynamic_cast<T*> (this);}

    template <typename T>
    const T*
    is_a () const {return dynamic_cast<const T*> (this);}

    const target*
    is_a (const char* n) const {
      return type ().is_a (n) ? this : nullptr;}

    // Unchecked cast.
    //
    template <typename T>
    T&
    as () {return static_cast<T&> (*this);}

    template <typename T>
    const T&
    as () const {return static_cast<const T&> (*this);}

    // Dynamic derivation to support define.
    //
    const target_type* derived_type = nullptr;

    const target_type&
    type () const
    {
      return derived_type != nullptr ? *derived_type : dynamic_type ();
    }

    virtual const target_type& dynamic_type () const = 0;
    static const target_type static_type;

    // RW access.
    //
    target&
    rw () const
    {
      assert (ctx.phase == run_phase::load);
      return const_cast<target&> (*this);
    }

  public:
    // Split the name leaf into target name (in place) and extension
    // (returned).
    //
    static optional<string>
    split_name (string&, const location&);

    // Combine the target name and extension into the name leaf.
    //
    // If the target type has the default extension, then "escape" the
    // existing extension if any.
    //
    static void
    combine_name (string&, const optional<string>&, bool default_extension);

    // Targets should be created via the targets set below.
    //
  public:
    target (context& c, dir_path d, dir_path o, string n)
        : ctx (c),
          dir (move (d)), out (move (o)), name (move (n)),
          vars (c, false /* global */),
          state (c) {}

    target (target&&) = delete;
    target& operator= (target&&) = delete;

    target (const target&) = delete;
    target& operator= (const target&) = delete;

    virtual
    ~target ();

    friend class target_set;
  };

  // All targets are from the targets set below.
  //
  inline bool
  operator== (const target& x, const target& y) {return &x == &y;}

  inline bool
  operator!= (const target& x, const target& y) {return !(x == y);}

  ostream&
  operator<< (ostream&, const target&);

  // Sometimes it is handy to "mark" a pointer to a target (for example, in
  // prerequisite_targets). We use the last 2 bits in a pointer for that (aka
  // the "bit stealing" technique). Note that the pointer needs to be unmarked
  // before it can be usable so care must be taken in the face of exceptions,
  // etc.
  //
  void
  mark (const target*&, uint8_t = 1);

  uint8_t
  marked (const target*); // Can be used as a predicate or to get the mark.

  uint8_t
  unmark (const target*&);

  // Helper for dealing with the prerequisite inclusion/exclusion (the
  // 'include' buildfile variable, see var_include in context.hxx).
  //
  // Note that the include(prerequisite_member) overload is also provided.
  //
  // @@ Maybe this filtering should be incorporated into *_prerequisites() and
  // *_prerequisite_members() logic? Could make normal > adhoc > excluded and
  // then pass the "threshold".
  //
  include_type
  include (action,
           const target&,
           const prerequisite&,
           const target* = nullptr);

  // A "range" that presents the prerequisites of a group and one of
  // its members as one continuous sequence, or, in other words, as
  // if they were in a single container. The group's prerequisites
  // come first followed by the member's. If you need to see them
  // in the other direction, iterate in reverse, for example:
  //
  // for (prerequisite& p: group_prerequisites (t))
  //
  // for (prerequisite& p: reverse_iterate (group_prerequisites (t))
  //
  // Note that in this case the individual elements of each list will
  // also be traversed in reverse, but that's what you usually want,
  // anyway.
  //
  // Note that you either should be iterating over a locked target (e.g., in
  // rule's match() or apply()) or you should call resolve_group().
  //
  class group_prerequisites
  {
  public:
    explicit
    group_prerequisites (const target& t);

    group_prerequisites (const target& t, const target* g);

    using prerequisites_type = target::prerequisites_type;
    using base_iterator      = prerequisites_type::const_iterator;

    struct iterator
    {
      using value_type        = base_iterator::value_type;
      using pointer           = base_iterator::pointer;
      using reference         = base_iterator::reference;
      using difference_type   = base_iterator::difference_type;
      using iterator_category = std::bidirectional_iterator_tag;

      iterator () {}
      iterator (const target* t,
                const target* g,
                const prerequisites_type* c,
                base_iterator i): t_ (t), g_ (g), c_ (c), i_ (i) {}

      iterator&
      operator++ ();

      iterator
      operator++ (int) {iterator r (*this); operator++ (); return r;}

      iterator&
      operator-- ();

      iterator
      operator-- (int) {iterator r (*this); operator-- (); return r;}

      reference operator* () const {return *i_;}
      pointer operator-> () const {return i_.operator -> ();}

      friend bool
      operator== (const iterator& x, const iterator& y)
      {
        return x.t_ == y.t_ && x.g_ == y.g_ && x.c_ == y.c_ && x.i_ == y.i_;
      }

      friend bool
      operator!= (const iterator& x, const iterator& y) {return !(x == y);}

    private:
      const target*             t_ = nullptr;
      const target*             g_ = nullptr;
      const prerequisites_type* c_ = nullptr;
      base_iterator             i_;
    };

    using reverse_iterator = std::reverse_iterator<iterator>;

    iterator
    begin () const;

    iterator
    end () const;

    reverse_iterator
    rbegin () const {return reverse_iterator (end ());}

    reverse_iterator
    rend () const {return reverse_iterator (begin ());}

    size_t
    size () const;

  private:
    const target& t_;
    const target* g_;
  };

  // A member of a prerequisite. If 'member' is NULL, then this is the
  // prerequisite itself. Otherwise, it is its member. In this case
  // 'prerequisite' still refers to the prerequisite.
  //
  struct prerequisite_member
  {
    using scope_type = build2::scope;
    using target_type = build2::target;
    using prerequisite_type = build2::prerequisite;
    using target_type_type = build2::target_type;

    const prerequisite_type& prerequisite;
    const target_type* member;

    template <typename T>
    bool
    is_a () const
    {
      return member != nullptr
        ? member->is_a<T> () != nullptr
        : prerequisite.is_a<T> ();
    }

    bool
    is_a (const target_type_type& tt) const
    {
      return member != nullptr
        ? member->is_a (tt) != nullptr
        : prerequisite.is_a (tt);
    }

    prerequisite_key
    key () const;

    const target_type_type&
    type () const
    {
      return member != nullptr ? member->type () : prerequisite.type;
    }

    const string&
    name () const
    {
      return member != nullptr ? member->name : prerequisite.name;
    }

    const dir_path&
    dir () const
    {
      return member != nullptr ? member->dir : prerequisite.dir;
    }

    const optional<project_name>&
    proj () const
    {
      // Member cannot be project-qualified.
      //
      return member != nullptr ? nullopt_project_name : prerequisite.proj;
    }

    const scope_type&
    scope () const
    {
      return member != nullptr ? member->base_scope () : prerequisite.scope;
    }

    const target_type&
    search (const target_type& t) const
    {
      return member != nullptr ? *member : build2::search (t, prerequisite);
    }

    const target_type*
    search_existing () const
    {
      return member != nullptr
        ? member
        : build2::search_existing (prerequisite);
    }

    const target_type*
    load (memory_order mo = memory_order_consume)
    {
      return member != nullptr ? member : prerequisite.target.load (mo);
    }

    // Return as a new prerequisite instance.
    //
    prerequisite_type
    as_prerequisite () const;
  };

  // It is often stored as the target's auxiliary data so make sure there is
  // no destructor overhead.
  //
  static_assert (std::is_trivially_destructible<prerequisite_member>::value,
                 "prerequisite_member is not trivially destructible");

  inline ostream&
  operator<< (ostream& os, const prerequisite_member& pm)
  {
    return os << pm.key ();
  }

  inline include_type
  include (action a, const target& t, const prerequisite_member& pm)
  {
    return include (a, t, pm.prerequisite, pm.member);
  }

  // A "range" that presents a sequence of prerequisites (e.g., from
  // group_prerequisites()) as a sequence of prerequisite_member's. For each
  // group prerequisite you will "see" either the prerequisite itself or all
  // its members, depending on the default iteration mode of the target group
  // type (ad hoc groups are never implicitly see through since one can only
  // safely access members after a synchronous match). You can skip the
  // rest of the group members with leave_group() and you can force iteration
  // over the members with enter_group(). Usage:
  //
  // for (prerequisite_member pm: prerequisite_members (a, ...))
  //
  // Where ... can be:
  //
  //   t.prerequisites
  //   reverse_iterate(t.prerequisites)
  //   group_prerequisites (t)
  //   reverse_iterate (group_prerequisites (t))
  //
  // But use shortcuts instead:
  //
  // prerequisite_members (a, t)
  // reverse_prerequisite_members (a, t)
  // group_prerequisite_members (a, t)
  // reverse_group_prerequisite_members (a, t)
  //
  template <typename R>
  class prerequisite_members_range;

  // See-through group members iteration mode. Ad hoc members must always
  // be entered explicitly.
  //
  enum class members_mode
  {
    always, // Iterate over members, assert if not resolvable.
    maybe,  // Iterate over members if resolvable, group otherwise.
    never   // Iterate over group (can still use enter_group()).
  };

  template <typename R>
  inline prerequisite_members_range<R>
  prerequisite_members (action a, const target& t,
                        R&& r,
                        members_mode m = members_mode::always)
  {
    return prerequisite_members_range<R> (a, t, forward<R> (r), m);
  }

  template <typename R>
  class prerequisite_members_range
  {
  public:
    prerequisite_members_range (action a, const target& t,
                                R&& r,
                                members_mode m)
        : a_ (a), t_ (t), mode_ (m), r_ (forward<R> (r)), e_ (r_.end ()) {}

    using base_iterator = decltype (declval<R> ().begin ());

    struct iterator
    {
      using value_type        = prerequisite_member;
      using pointer           = const value_type*;
      using reference         = const value_type&;
      using difference_type   = typename base_iterator::difference_type;
      using iterator_category = std::forward_iterator_tag;

      iterator (): r_ (nullptr) {}
      iterator (const prerequisite_members_range* r, const base_iterator& i)
          : r_ (r), i_ (i), g_ {nullptr, 0}, k_ (nullptr)
      {
        if (r_->mode_ != members_mode::never &&
            i_ != r_->e_                     &&
            i_->type.see_through)
          switch_mode ();
      }

      iterator& operator++ ();
      iterator operator++ (int) {iterator r (*this); operator++ (); return r;}

      // Skip iterating over the rest of this group's members, if any. Note
      // that the only valid operation after this call is to increment the
      // iterator.
      //
      void
      leave_group ();

      // Iterate over this group's members. Return false if the member
      // information is not available. Similar to leave_group(), you should
      // increment the iterator after calling this function (provided it
      // returned true).
      //
      bool
      enter_group ();

      // Return true if the next element is this group's members. Normally
      // used to iterate over group members only, for example:
      //
      // for (...; ++i)
      // {
      //   if (i->prerequisite.type.see_through)
      //   {
      //     for (i.enter_group (); i.group (); )
      //     {
      //       ++i;
      //       ...
      //     }
      //   }
      // }
      //
      bool
      group () const;

      value_type operator* () const
      {
        const target* t (k_ != nullptr ? k_:
                         g_.count != 0 ? g_.members[j_ - 1] : nullptr);

        return value_type {*i_, t};
      }

      pointer operator-> () const
      {
        static_assert (
          std::is_trivially_destructible<value_type>::value,
          "prerequisite_member is not trivially destructible");

        const target* t (k_ != nullptr ? k_:
                         g_.count != 0 ? g_.members[j_ - 1] : nullptr);

        return new (&m_) value_type {*i_, t};
      }

      friend bool
      operator== (const iterator& x, const iterator& y)
      {
        return x.i_ == y.i_ &&
          x.g_.count == y.g_.count &&
          (x.g_.count == 0 || x.j_ == y.j_) &&
          x.k_ == y.k_;
      }

      friend bool
      operator!= (const iterator& x, const iterator& y) {return !(x == y);}

      // What we have here is a state for three nested iteration modes (and
      // no, I am not proud of it). The innermost mode is iteration over an ad
      // hoc group (k_). Then we have iteration over a normal group (g_ and
      // j_). Finally, at the outer level, we have the range itself (i_).
      //
      // Also, the enter/leave group support is full of ugly, special cases.
      //
    private:
      void
      switch_mode ();

      group_view
      resolve_members (const prerequisite&);

    private:
      const prerequisite_members_range* r_;
      base_iterator i_;
      group_view g_;
      size_t j_;        // 1-based index, to support enter_group().
      const target* k_; // Current member of ad hoc group or NULL.
      mutable typename std::aligned_storage<sizeof (value_type),
                                            alignof (value_type)>::type m_;
    };

    iterator
    begin () const {return iterator (this, r_.begin ());}

    iterator
    end () const {return iterator (this, e_);}

  private:
    action a_;
    const target& t_;
    members_mode mode_;
    R r_;
    base_iterator e_;
  };

  // prerequisite_members(t.prerequisites ())
  //
  auto
  prerequisite_members (action a, const target& t,
                        members_mode m = members_mode::always);

  // prerequisite_members(reverse_iterate(t.prerequisites ()))
  //
  auto
  reverse_prerequisite_members (action a, const target& t,
                                members_mode m = members_mode::always);

  // prerequisite_members(group_prerequisites (t))
  //
  inline auto
  group_prerequisite_members (action a, target& t,
                              members_mode m = members_mode::always)
  {
    return prerequisite_members (a, t, group_prerequisites (t), m);
  }

  inline auto
  group_prerequisite_members (action a, const target& t,
                              members_mode m = members_mode::always)
  {
    return prerequisite_members (a, t, group_prerequisites (t), m);
  }

  // prerequisite_members(reverse_iterate (group_prerequisites (t)))
  //
  inline auto
  reverse_group_prerequisite_members (action a, target& t,
                                      members_mode m = members_mode::always)
  {
    return prerequisite_members (
      a, t, reverse_iterate (group_prerequisites (t)), m);
  }

  inline auto
  reverse_group_prerequisite_members (action a, const target& t,
                                      members_mode m = members_mode::always)
  {
    return prerequisite_members (
      a, t, reverse_iterate (group_prerequisites (t)), m);
  }

  // A target with an unspecified extension is considered equal to the one
  // with the specified one. And when we find a target with an unspecified
  // extension via a key with the specified one, we update the extension,
  // essentially modifying the map's key. To make this work we use a hash
  // map. The key's hash ignores the extension, so the hash will stay stable
  // across extension updates.
  //
  // Note also that once the extension is specified, it becomes immutable.
  //
  class LIBBUILD2_SYMEXPORT target_set
  {
  public:
    using map_type = std::unordered_map<target_key, unique_ptr<target>>;

    // Return existing target or NULL.
    //
    const target*
    find (const target_key& k, tracer& trace) const;

    const target*
    find (const target_type& type,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const optional<string>& ext,
          tracer& trace) const
    {
      return find (target_key {&type, &dir, &out, &name, ext}, trace);
    }

    template <typename T>
    const T*
    find (const target_type& type,
          const dir_path& dir,
          const dir_path& out,
          const string& name,
          const optional<string>& ext,
          tracer& trace) const
    {
      return static_cast<const T*> (find (type, dir, out, name, ext, trace));
    }

    // As above but ignore the extension.
    //
    const target*
    find (const target_type& type,
          const dir_path& dir,
          const dir_path& out,
          const string& name) const
    {
      slock l (mutex_);
      auto i (map_.find (target_key {&type, &dir, &out, &name, nullopt}));
      return i != map_.end () ? i->second.get () : nullptr;
    }

    template <typename T>
    const T*
    find (const dir_path& dir, const dir_path& out, const string& name) const
    {
      return static_cast<const T*> (find (T::static_type, dir, out, name));
    }

    // If the target was inserted, keep the map exclusive-locked and return
    // the lock. In this case, the target is effectively still being created
    // since nobody can see it until the lock is released.
    //
    pair<target&, ulock>
    insert_locked (const target_type&,
                   dir_path dir,
                   dir_path out,
                   string name,
                   optional<string> ext,
                   bool implied,
                   tracer&);

    pair<target&, bool>
    insert (const target_type& tt,
            dir_path dir,
            dir_path out,
            string name,
            optional<string> ext,
            bool implied,
            tracer& t)
    {
      auto p (insert_locked (tt,
                             move (dir),
                             move (out),
                             move (name),
                             move (ext),
                             implied,
                             t));

      return pair<target&, bool> (p.first, p.second.owns_lock ());
    }

    // Note that the following versions always enter implied targets.
    //
    template <typename T>
    T&
    insert (const target_type& tt,
            dir_path dir,
            dir_path out,
            string name,
            optional<string> ext,
            tracer& t)
    {
      return insert (tt,
                     move (dir),
                     move (out),
                     move (name),
                     move (ext),
                     true,
                     t).first.template as<T> ();
    }

    template <typename T>
    T&
    insert (const dir_path& dir,
            const dir_path& out,
            const string& name,
            const optional<string>& ext,
            tracer& t)
    {
      return insert<T> (T::static_type, dir, out, name, ext, t);
    }

    template <typename T>
    T&
    insert (const dir_path& dir,
            const dir_path& out,
            const string& name,
            tracer& t)
    {
      return insert<T> (dir, out, name, nullopt, t);
    }

    // Note: not MT-safe so can only be used during serial execution.
    //
  public:
    using iterator = butl::map_iterator_adapter<map_type::const_iterator>;

    iterator begin () const {return map_.begin ();}
    iterator end ()   const {return map_.end ();}

    void
    clear () {map_.clear ();}

  private:
    friend class target; // Access to mutex.
    friend class context;

    explicit
    target_set (context& c): ctx (c) {}

    context& ctx;

    mutable shared_mutex mutex_;
    map_type map_;
  };

  // Modification time-based target.
  //
  class LIBBUILD2_SYMEXPORT mtime_target: public target
  {
  public:
    using target::target;

    // Modification time is an "atomic cash". That is, it can be set at any
    // time (including on a const instance) and we assume everything will be
    // ok regardless of the order in which racing updates happen because we do
    // not modify the external state (which is the source of timestemps) while
    // updating the internal.
    //
    // The modification time is reserved for the inner operation thus there is
    // no action argument.
    //
    // The rule for groups that utilize target_state::group is as follows: if
    // it has any members that are mtime_targets, then the group should be
    // mtime_target and the members get the mtime from it. During match and
    // execute the target should be synchronized.
    //
    // Note that this function can be called before the target is matched in
    // which case the value always comes from the target itself. In other
    // words, that group logic only kicks in once the target is matched.
    //
    timestamp
    mtime () const;

    // Note also that while we can cache the mtime, it may be ignored if the
    // target state is set to group (see above).
    //
    void
    mtime (timestamp) const;

    // If the mtime is unknown, then load it from the filesystem also caching
    // the result.
    //
    // Note: can only be called during executing and must not be used if the
    // target state is group.
    //
    timestamp
    load_mtime (const path&) const;

    // Return true if this target is newer than the specified timestamp.
    //
    // Note: can only be called during execute on a synchronized target.
    //
    bool
    newer (timestamp) const;

  public:
    static const target_type static_type;

  protected:

    // Complain if timestamp is not lock-free unless we were told non-lock-
    // free is ok.
    //
#ifndef LIBBUILD2_ATOMIC_NON_LOCK_FREE
    // C++17:
    //
    // static_assert (atomic<timestamp::rep>::is_always_lock_free,
    //               "timestamp is not lock-free on this architecture");
    //
#if !defined(ATOMIC_LLONG_LOCK_FREE) || ATOMIC_LLONG_LOCK_FREE != 2
#  error timestamp is not lock-free on this architecture
#endif
#endif

    // Note that the value is not used to synchronize any other state so we
    // use the release-consume ordering (i.e., we are only interested in the
    // mtime value being synchronized).
    //
    // Store it as an underlying representation (normally int64_t) since
    // timestamp is not usable with atomic (non-noexcept default ctor).
    //
    mutable atomic<timestamp::rep> mtime_ {timestamp_unknown_rep};
  };

  // Filesystem path-based target.
  //
  class LIBBUILD2_SYMEXPORT path_target: public mtime_target
  {
  public:
    using mtime_target::mtime_target;

    typedef build2::path path_type;

    // Target path is an "atomic consistent cash". That is, it can be set at
    // any time (including on a const instance) but any subsequent updates
    // must set the same path. Or, in other words, once the path is set, it
    // never changes.
    //
    // An empty path may signify special unknown/undetermined/unreal location
    // (for example, a binless library or an installed import library -- we
    // know the DLL is there, just not exactly where). In this case you could
    // also set its mtime to timestamp_unreal (but don't have to, if a real
    // timestamp can be derived, for example, the from the import library in
    // the DLL case above).
    //
    // Note, however, that a target with timestamp_unreal does not have to
    // have an empty path. One consequence of this arrangement (assigned path
    // with unreal timestamp) is that the timestamp of such a target when used
    // as a prerequisite won't affect the dependent's target out-of-date-ness.
    //
    // We used to return a pointer to properly distinguish between not set and
    // empty but that proved too tedious to work with. So now we return empty
    // path both when not set (which will be empty_path so you can distinguish
    // the two case if you really want to) and when set to empty. Note that
    // this means there could be a race between path and mtime (unless you
    // lock the target in some other way; see file_rule) so in this case it
    // makes sense to set the timestamp first.
    //
    const path_type&
    path () const;

    const path_type&
    path (path_type) const;

    timestamp
    load_mtime () const;

    // Derive a path from target's dir, name, and, if set, ext. If ext is not
    // set, try to derive it using the target type extension function and
    // fallback to default_ext, if specified. In both cases also update the
    // target's extension (this becomes important if later we need to reliably
    // determine whether this file has an extension; think hxx{foo.bar.} and
    // hxx{*}:extension is empty).
    //
    // If name_prefix is not NULL, add it before the name part and after the
    // directory. Similarly, if name_suffix is not NULL, add it after the name
    // part and before the extension. And if extra_ext is not NULL, then add
    // it as an extra extension (think libfoo.so.1.2.3).
    //
    // Finally, if the path was already assigned to this target, then this
    // function verifies that the two are the same.
    //
    const path_type&
    derive_path (const char* default_ext = nullptr,
                 const char* name_prefix = nullptr,
                 const char* name_suffix = nullptr,
                 const char* extra_ext = nullptr);

    // This version can be used to derive the path from another target's path
    // by adding another extension.
    //
    const path_type&
    derive_path (path_type base,
                 const char* default_ext = nullptr,
                 const char* extra_ext = nullptr);

    // As above but only derives (and returns) the extension (empty means no
    // extension used).
    //
    const string&
    derive_extension (const char* default_ext = nullptr)
    {
      return *derive_extension (false, default_ext);
    }

    // As above but if search is true then look for the extension as if it was
    // a prerequisite, not a target. In this case, if no extension can be
    // derived, return NULL instead of failing (like search_existing_file()).
    //
    const string*
    derive_extension (bool search, const char* default_ext = nullptr);

    // Const versions of the above that can be used on unlocked targets. Note
    // that here we don't allow providing any defaults since you probably
    // should only use this version if everything comes from the target itself
    // (and is therefore atomic).
    //
    const path_type&
    derive_path () const
    {
      return const_cast<path_target*> (this)->derive_path (); // MT-aware.
    }

    const string&
    derive_extension () const
    {
      return const_cast<path_target*> (this)->derive_extension (); // MT-aware.
    }

  public:
    static const target_type static_type;

  private:
    // Note that the state is also used to synchronize the path value so
    // we use the release-acquire ordering.
    //
    // 0 - absent
    // 1 - being set
    // 2 - present
    //
    mutable atomic<uint8_t> path_state_ {0};
    mutable path_type path_;
  };

  // File target.
  //
  class LIBBUILD2_SYMEXPORT file: public path_target
  {
  public:
    using path_target::path_target;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // Alias target. It represents a list of targets (its prerequisites)
  // as a single "name".
  //
  class LIBBUILD2_SYMEXPORT alias: public target
  {
  public:
    using target::target;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // Directory target. Note that this is not a filesystem directory
  // but rather an alias target with the directory name. For actual
  // filesystem directory (creation), see fsdir.
  //
  class LIBBUILD2_SYMEXPORT dir: public alias
  {
  public:
    using alias::alias;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}

  public:
    template <typename K>
    static const target*
    search_implied (const scope&, const K&, tracer&);

    // Return true if the implied buildfile is plausible for the specified
    // subdirectory of a project with the specified root scope. That is, there
    // is a buildfile in at least one of its subdirectories. Note that the
    // directory must exist.
    //
    static bool
    check_implied (const scope& root, const dir_path&);

  private:
    static prerequisites_type
    collect_implied (const scope&);
  };

  // While a filesystem directory is mtime-based, the semantics is not very
  // useful in our case. In particular, if another target depends on fsdir{},
  // then all that's desired is the creation of the directory if it doesn't
  // already exist. In particular, we don't want to update the target just
  // because some unrelated entry was created in that directory.
  //
  class LIBBUILD2_SYMEXPORT fsdir: public target
  {
  public:
    using target::target;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // Executable file.
  //
  class LIBBUILD2_SYMEXPORT exe: public file
  {
  public:
    using file::file;

    using process_path_type = build2::process_path;

    // Return the process path of this executable target. Normally it will be
    // the absolute path returned by path() but can also be something custom
    // if, for example, the target was found via a PATH search (see import for
    // details). The idea is to use this path if we need to execute the target
    // in which case, for the above example, we will see a short (recall) path
    // instead of the absolute one in diagnostics.
    //
    process_path_type
    process_path () const;

    // Note that setting the custom process path is not MT-safe and must be
    // done while holding the insertion lock.
    //
    void
    process_path (process_path_type);

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}

  private:
    process_path_type process_path_;
  };

  class LIBBUILD2_SYMEXPORT buildfile: public file
  {
  public:
    using file::file;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // Common documentation file targets.
  //
  class LIBBUILD2_SYMEXPORT doc: public file
  {
  public:
    using file::file;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // The problem with man pages is this: different platforms have
  // different sets of sections. What seems to be the "sane" set
  // is 1-9 (Linux and BSDs). SysV (e.g., Solaris) instead maps
  // 8 to 1M (system administration). The section determines two
  // things: the directory where the page is installed (e.g.,
  // /usr/share/man/man1) as well as the extension of the file
  // (e.g., test.1). Note also that there could be sub-sections,
  // e.g., 1p (for POSIX). Such a page would still go into man1
  // but will have the .1p extension (at least that's what happens
  // on Linux). The challenge is to somehow handle this in a
  // portable manner. So here is the plan:
  //
  // First of all, we have the man{} target type which can be used
  // for a custom man page. That is, you can have any extension and
  // install it anywhere you please:
  //
  // man{foo.X}: install = man/manX
  //
  // Then we have man1..9{} target types which model the "sane"
  // section set and that would be automatically installed into
  // correct locations on other platforms. In other words, the
  // idea is that you should be able to have the foo.8 file,
  // write man8{foo} and have it installed as man1m/foo.1m on
  // some SysV host.
  //
  // Re-mapping the installation directory is easy: to help with
  // that we have assigned install.man1..9 directory names. The
  // messy part is to change the extension. It seems the only
  // way to do that would be to have special logic for man pages
  // in the generic install rule. @@ This is still a TODO.
  //
  // Note that handling subsections with man1..9{} is easy, we
  // simply specify the extension explicitly, e.g., man{foo.1p}.
  //
  class LIBBUILD2_SYMEXPORT man: public doc
  {
  public:
    using doc::doc;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  class LIBBUILD2_SYMEXPORT man1: public man
  {
  public:
    using man::man;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // We derive manifest from doc rather than file so that it get automatically
  // installed into the same place where the rest of the documentation goes.
  // If you think about it, it's kind of a documentation, similar to (but
  // better than) the version file that many projects come with.
  //
  class LIBBUILD2_SYMEXPORT manifest: public doc
  {
  public:
    using doc::doc;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // Common implementation of the target factory, extension, and search
  // functions.
  //
  template <typename T>
  target*
  target_factory (context& c,
                  const target_type&, dir_path d, dir_path o, string n)
  {
    return new T (c, move (d), move (o), move (n));
  }

  // Return fixed target extension unless one was specified.
  //
  template <const char* ext>
  const char*
  target_extension_fix (const target_key&, const scope*);

  template <const char* ext>
  bool
  target_pattern_fix (const target_type&, const scope&,
                      string&, optional<string>&, const location&,
                      bool);

  // Get the extension from the `extension` variable or use the default if
  // none set. If the default is NULL, then return NULL.
  //
  template <const char* def>
  optional<string>
  target_extension_var (const target_key&, const scope&, const char*, bool);

  template <const char* def>
  bool
  target_pattern_var (const target_type&, const scope&,
                      string&, optional<string>&, const location&,
                      bool);

  // Target print functions.
  //

  // Target type uses the extension but it is fixed and there is no use
  // printing it (e.g., man1{}).
  //
  LIBBUILD2_SYMEXPORT void
  target_print_0_ext_verb (ostream&, const target_key&);

  // Target type uses the extension and there is normally no default so it
  // should be printed (e.g., file{}).
  //
  LIBBUILD2_SYMEXPORT void
  target_print_1_ext_verb (ostream&, const target_key&);

  // The default behavior, that is, look for an existing target in the
  // prerequisite's directory scope.
  //
  LIBBUILD2_SYMEXPORT const target*
  target_search (const target&, const prerequisite_key&);

  // First look for an existing target as above. If not found, then look
  // for an existing file in the target-type-specific list of paths.
  //
  LIBBUILD2_SYMEXPORT const target*
  file_search (const target&, const prerequisite_key&);
}

#include <libbuild2/target.ixx>
#include <libbuild2/target.txx>

#endif // LIBBUILD2_TARGET_HXX
