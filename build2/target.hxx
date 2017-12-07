// file      : build2/target.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TARGET_HXX
#define BUILD2_TARGET_HXX

#include <iterator>     // tags, etc.
#include <type_traits>  // aligned_storage
#include <unordered_map>

#include <libbutl/multi-index.mxx> // map_iterator_adapter

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/scope.hxx>
#include <build2/variable.hxx>
#include <build2/operation.hxx>
#include <build2/target-key.hxx>
#include <build2/target-type.hxx>
#include <build2/target-state.hxx>
#include <build2/prerequisite.hxx>

namespace build2
{
  class rule;
  class scope;
  class target;

  extern size_t current_on; // From <build/context>.

  // From <build2/algorithm.hxx>.
  //
  const target& search (const target&, const prerequisite&);
  const target* search_existing (const prerequisite&);

  // Recipe.
  //
  // The returned target state is normally changed or unchanged. If there is
  // an error, then the recipe should throw failed rather than returning (this
  // is the only exception that a recipe can throw).
  //
  // The return value of the recipe is used to update the target state. If it
  // is target_state::group then the target's state is the group's state.
  //
  // The recipe may also return postponed in which case the target state is
  // assumed to be unchanged (normally this means a prerequisite was postponed
  // and while the prerequisite will be re-examined via another dependency,
  // this target is done).
  //
  // Note that max size for the "small capture optimization" in std::function
  // ranges (in pointer sizes) from 0 (GCC prior to 5) to 2 (GCC 5) to 6 (VC
  // 14u2). With the size ranging (in bytes for 64-bit target) from 32 (GCC)
  // to 64 (VC).
  //
  using recipe_function = target_state (action, const target&);
  using recipe = function<recipe_function>;

  // Commonly-used recipes. The default recipe executes the action on
  // all the prerequisites in a loop, skipping ignored. Specifically,
  // for actions with the "first" execution mode, it calls
  // execute_prerequisites() while for those with the "last" mode --
  // reverse_execute_prerequisites(); see <build2/operation.hxx>,
  // <build2/algorithm.hxx> for details. The group recipe call's the group's
  // recipe.
  //
  extern const recipe empty_recipe;
  extern const recipe noop_recipe;
  extern const recipe default_recipe;
  extern const recipe group_recipe;

  target_state
  noop_action (action, const target&); // Defined in <build2/algorithm.hxx>.

  target_state
  group_action (action, const target&); // Defined in <build2/algorithm.hxx>.

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

    prerequisite_target (const target_type* t, uintptr_t d = 0)
        : target (t), data (d) {}

    operator const target_type*&  ()       {return target;}
    operator const target_type*   () const {return target;}
    const target_type* operator-> () const {return target;}

    const target_type* target;
    uintptr_t          data;
  };
  using prerequisite_targets = vector<prerequisite_target>;

  // Target.
  //
  class target
  {
    optional<string>* ext_; // Reference to value in target_key.

  public:
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
    const dir_path   dir;  // Absolute and normalized.
    const dir_path   out;  // Empty or absolute and normalized.
    const string     name;

    const string* ext () const; // Return NULL if not specified.
    const string& ext (string);

    const dir_path&
    out_dir () const {return out.empty () ? dir : out;}

    // Target group to which this target belongs, if any. Note that we assume
    // that the group and all its members are in the same scope (for example,
    // in variable lookup). We also don't support nested groups (with a small
    // exception for ad hoc groups; see below).
    //
    // The semantics of the interaction between the group and its members and
    // what it means to, say, update the group, is unspecified and is
    // determined by the group's type. In particular, a group can be created
    // out of member types that have no idea they are part of this group
    // (e.g., cli.cxx{}).
    //
    // Normally, however, there are two kinds of groups: "alternatives" and
    // "combination". In an alternatives group, normally one of the members is
    // selected when the group is mentioned as a prerequisite with, perhaps,
    // an exception for special rules, like aliases, where it makes more sense
    // to treat the group as a whole. In this case we say that the rule
    // "semantically recognizes" the group and picks some of its members.
    //
    // Updating an alternatives group as a whole can mean updating some subset
    // of its members (e.g., lib{}). Or the group may not support this at all
    // (e.g., obj{}).
    //
    // In a combination group, when a group is updated, normally all members
    // are updates (and usually with a single command), though there could be
    // some members that are omitted, depending on the configuration (e.g., an
    // inline file not/being generated). When a combination group is mentioned
    // as a prerequisite, the rule is usually interested in the individual
    // members rather than the whole group. For example, a C++ compile rule
    // would like to "see" the ?xx{} members when it gets a cli.cxx{} group.
    //
    // Which brings us to the group iteration mode. The target type contains a
    // member called see_through that indicates whether the default iteration
    // mode for the group should be "see through"; that is, whether we see the
    // members or the group itself. For the iteration support itself, see the
    // *_prerequisite_members() machinery below.
    //
    // In a combination group we usually want the state (and timestamp; see
    // mtime()) for members to come from the group. This is achieved with the
    // special target_state::group state. You would normally also use the
    // group_recipe for group members.
    //
    const target* group = nullptr;


    // What has been described above is a "normal" group. That is, there is
    // a dedicated target type that explicitly serves as a group and there
    // is an explicit mechanism for discovering the group's members.
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
    // that any target can be turned (by the rule that matched it) into an ad
    // hoc group by chaining several targets. Ad hoc groups have a more
    // restricted semantics compared to the normal groups. In particular:
    //
    // - The ad hoc group itself is in a sense its first/primary target.
    //
    // - Group member's recipes should be set to group_recipe by the group's
    //   rule.
    //
    // - Members are discovered lazily, they are only known after the group's
    //   rule's apply() call.
    //
    // - Members cannot be used as prerequisites but can be used as targets
    // - (e.g., to set variables, etc).
    //
    // - Members don't have prerequisites.
    //
    // - Ad hoc group cannot have sub groups (of any kind) though an ad hoc
    //   group can be a sub group of a normal group.
    //
    // - Member variable lookup skips the ad hoc group (since the group is
    //   the first member, this is normally what we want).
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
    using action_type = build2::action;

    // You should not call this function directly; rather use
    // resolve_group_members() from <build2/algorithm.hxx>.
    //
    virtual group_view
    group_members (action_type) const;

    // Note that the returned key "tracks" the target (except for the
    // extension).
    //
    target_key
    key () const;

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

    // Check if there are any prerequisites, taking into account group
    // prerequisites.
    //
    bool
    has_prerequisites () const
    {
      return !prerequisites ().empty () ||
        (group != nullptr && !group->prerequisites ().empty ());
    }

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
  public:
    variable_map vars;

    // Lookup, including in groups to which this target belongs and then in
    // outer scopes (including target type/pattern-specific variables). If you
    // only want to lookup in this target, do it on the variable map directly
    // (and note that there will be no overrides).
    //
    lookup
    operator[] (const variable& var) const
    {
      return find (var).first;
    }

    lookup
    operator[] (const variable* var) const // For cached variables.
    {
      assert (var != nullptr);
      return operator[] (*var);
    }

    lookup
    operator[] (const string& name) const
    {
      const variable* var (var_pool.find (name));
      return var != nullptr ? operator[] (*var) : lookup ();
    }

    // As above but also return the depth at which the value is found. The
    // depth is calculated by adding 1 for each test performed. So a value
    // that is from the target will have depth 1. That from the group -- 2.
    // From the innermost scope's target type/patter-specific variables --
    // 3. From the innermost scope's variables -- 4. And so on.  The idea is
    // that given two lookups from the same target, we can say which one came
    // earlier. If no value is found, then the depth is set to ~0.
    //
    //
    pair<lookup, size_t>
    find (const variable& var) const
    {
      auto p (find_original (var));
      return var.override == nullptr
        ? p
        : base_scope ().find_override (var, move (p), true);
    }

    // If target_only is true, then only look in target and its target group
    // without continuing in scopes.
    //
    pair<lookup, size_t>
    find_original (const variable&, bool target_only = false) const;

    // Return a value suitable for assignment. See scope for details.
    //
    value&
    assign (const variable& var) {return vars.assign (var);}

    // Return a value suitable for appending. See scope for details.
    //
    value&
    append (const variable&);

    // A target that is not (yet) entered as part of a real dependency
    // declaration (for example, that is entered as part of a target-specific
    // variable assignment, dependency extraction, etc) is called implied.
    //
    // The implied flag should only be cleared during the load phase via the
    // MT-safe target_set::insert().
    //
  public:
    bool implied;

    // Target state.
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
    // For match we have a further complication in that we may re-match the
    // target and override with a "stronger" recipe thus re-setting the state
    // from, say, applied back to touched.
    //
    // The target is said to be synchronized (in this thread) if we have
    // either observed the task count to reach applied or executed or we have
    // successfully changed it (via compare_exchange) to locked or busy. If
    // the target is synchronized, then we can access and modify (second case)
    // its state etc.
    //
    static const size_t offset_touched  = 1; // Target has been locked.
    static const size_t offset_tried    = 2; // Rule match has been tried.
    static const size_t offset_matched  = 3; // Rule has been matched.
    static const size_t offset_applied  = 4; // Rule has been applied.
    static const size_t offset_executed = 5; // Recipe has been executed.
    static const size_t offset_locked   = 6; // Fast (spin) lock.
    static const size_t offset_busy     = 7; // Slow (wait) lock.

    static size_t count_base     () {return 5 * (current_on - 1);}

    static size_t count_touched  () {return offset_touched  + count_base ();}
    static size_t count_tried    () {return offset_tried    + count_base ();}
    static size_t count_matched  () {return offset_matched  + count_base ();}
    static size_t count_applied  () {return offset_applied  + count_base ();}
    static size_t count_executed () {return offset_executed + count_base ();}
    static size_t count_locked   () {return offset_locked   + count_base ();}
    static size_t count_busy     () {return offset_busy     + count_base ();}

    mutable atomic_count task_count {0}; // Start offset_touched - 1.

    // This function should only be called during match if we have observed
    // (synchronization-wise) that this target has been matched (i.e., the
    // rule has been applied) for this action.
    //
    target_state
    matched_state (action a, bool fail = true) const;

    // See try_match().
    //
    pair<bool, target_state>
    try_matched_state (action a, bool fail = true) const;

    // This function should only be called during execution if we have
    // observed (synchronization-wise) that this target has been executed.
    //
    target_state
    executed_state (bool fail = true) const;

    // This function should only be called between match and execute while
    // running serially. It returns the group state for the "final" action
    // that has been matched (and that will be executed).
    //
    target_state
    serial_state (bool fail = true) const;

    // Number of direct targets that depend on this target in the current
    // operation. It is incremented during match and then decremented during
    // execution, before running the recipe. As a result, the recipe can
    // detect the last chance (i.e., last dependent) to execute the command
    // (see also the first/last execution modes in <operation>).
    //
    mutable atomic_count dependents;

  protected:
    // Return fail-untranslated (but group-translated) state assuming the
    // target is executed and synchronized.
    //
    target_state
    state () const;

    // Version that should be used during match after the target has been
    // matched for this action (see the recipe override).
    //
    // Indicate whether there is a rule match with the first half of the
    // result (see try_match()).
    //
    pair<bool, target_state>
    state (action a) const;

    // Return true if the state comes from the group. Target must be at least
    // matched.
    //
    bool
    group_state () const;

    // Raw state, normally not accessed directly.
    //
  public:
    target_state state_ = target_state::unknown;

    // Recipe.
    //
  public:
    using recipe_type = build2::recipe;
    using rule_type = build2::rule;

    action_type action; // Action the rule/recipe is for.

    // Matched rule (pointer to hint_rule_map element). Note that in case of a
    // direct recipe assignment we may not have a rule.
    //
    const pair<const string, reference_wrapper<const rule_type>>* rule;

    // Applied recipe.
    //
    recipe_type recipe_;

    // Note that the target must be locked in order to set the recipe.
    //
    void
    recipe (recipe_type);

    // After the target has been matched and synchronized, check if the target
    // is known to be unchanged. Used for optimizations during search & match.
    //
    bool
    unchanged (action_type a) const
    {
      return state (a).second == target_state::unchanged;
    }

    // Targets to which prerequisites resolve for this recipe. Note that
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
    mutable build2::prerequisite_targets prerequisite_targets;

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
    // Note that the recipe may modify the data.
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

  public:
    virtual
    ~target ();

    target (const target&) = delete;
    target& operator= (const target&) = delete;

    // The only way to create a target should be via the targets set below.
    //
  public:
    friend class target_set;

    target (dir_path d, dir_path o, string n)
        : dir (move (d)), out (move (o)), name (move (n)),
          vars (false /* global */) {}
  };

  // All targets are from the targets set below.
  //
  inline bool
  operator== (const target& x, const target& y) {return &x == &y;}

  inline bool
  operator!= (const target& x, const target& y) {return !(x == y);}

  inline ostream&
  operator<< (ostream& os, const target& t) {return os << t.key ();}

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
  // For constant iteration use const_group_prerequisites().
  //
  class group_prerequisites
  {
  public:
    explicit
    group_prerequisites (const target& t)
        : t_ (t),
          g_ (t_.group == nullptr                 ||
              t_.group->member != nullptr         || // Ad hoc group member.
              t_.group->prerequisites ().empty ()
              ? nullptr : t_.group) {}

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
      operator++ ()
      {
        if (++i_ == c_->end () && c_ != &t_->prerequisites ())
        {
          c_ = &t_->prerequisites ();
          i_ = c_->begin ();
        }
        return *this;
      }

      iterator
      operator++ (int) {iterator r (*this); operator++ (); return r;}

      iterator&
      operator-- ()
      {
        if (i_ == c_->begin () && c_ == &t_->prerequisites ())
        {
          c_ = &g_->prerequisites ();
          i_ = c_->end ();
        }

        --i_;
        return *this;
      }

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
    begin () const
    {
      auto& c ((g_ != nullptr ? *g_ : t_).prerequisites ());
      return iterator (&t_, g_, &c, c.begin ());
    }

    iterator
    end () const
    {
      auto& c (t_.prerequisites ());
      return iterator (&t_, g_, &c, c.end ());
    }

    reverse_iterator
    rbegin () const {return reverse_iterator (end ());}

    reverse_iterator
    rend () const {return reverse_iterator (begin ());}

    size_t
    size () const
    {
      return t_.prerequisites ().size () +
        (g_ != nullptr ? g_->prerequisites ().size () : 0);
    }

  private:
    const target& t_;
    const target* g_;
  };

  // A member of a prerequisite. If 'target' is NULL, then this is the
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
    const target_type* target;

    template <typename T>
    bool
    is_a () const
    {
      return target != nullptr
        ? target->is_a<T> () != nullptr
        : prerequisite.is_a<T> ();
    }

    bool
    is_a (const target_type_type& tt) const
    {
      return target != nullptr
        ? target->is_a (tt) != nullptr
        : prerequisite.is_a (tt);
    }

    prerequisite_key
    key () const
    {
      return target != nullptr
        ? prerequisite_key {prerequisite.proj, target->key (), nullptr}
        : prerequisite.key ();
    }

    const target_type_type&
    type () const
    {
      return target != nullptr ? target->type () : prerequisite.type;
    }

    const string&
    name () const
    {
      return target != nullptr ? target->name : prerequisite.name;
    }

    const dir_path&
    dir () const
    {
      return target != nullptr ? target->dir : prerequisite.dir;
    }

    const optional<string>&
    proj () const
    {
      // Target cannot be project-qualified.
      //
      return target != nullptr
        ? prerequisite_key::nullproj
        : prerequisite.proj;
    }

    const scope_type&
    scope () const
    {
      return target != nullptr ? target->base_scope () : prerequisite.scope;
    }

    const target_type&
    search (const target_type& t) const
    {
      return target != nullptr ? *target : build2::search (t, prerequisite);
    }

    const target_type*
    search_existing () const
    {
      return target != nullptr
        ? target
        : build2::search_existing (prerequisite);
    }

    const target_type*
    load (memory_order mo = memory_order_consume)
    {
      return target != nullptr ? target : prerequisite.target.load (mo);
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

  // prerequisite_members(t.prerequisites)
  //
  inline auto
  prerequisite_members (action a, target& t,
                        members_mode m = members_mode::always)
  {
    return prerequisite_members (a, t, t.prerequisites (), m);
  }

  inline auto
  prerequisite_members (action a, const target& t,
                        members_mode m = members_mode::always)
  {
    return prerequisite_members (a, t, t.prerequisites (), m);
  }

  // prerequisite_members(reverse_iterate(t.prerequisites))
  //
  inline auto
  reverse_prerequisite_members (action a, target& t,
                                members_mode m = members_mode::always)
  {
    return prerequisite_members (a, t, reverse_iterate (t.prerequisites ()), m);
  }

  inline auto
  reverse_prerequisite_members (action a, const target& t,
                                members_mode m = members_mode::always)
  {
    return prerequisite_members (a, t, reverse_iterate (t.prerequisites ()), m);
  }

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
  class target_set
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

    mutable shared_mutex mutex_;
    map_type map_;
  };

  extern target_set targets;

  // Modification time-based target.
  //
  class mtime_target: public target
  {
  public:
    using target::target;

    // Modification time is an "atomic cash". That is, it can be set at any
    // time and we assume everything will be ok regardless of the order in
    // which racing updates happen because we do not modify the external state
    // (which is the source of timestemps) while updating the internal.
    //
    // The rule for groups that utilize target_state::group is as follows: if
    // it has any members that are mtime_targets, then the group should be
    // mtime_target and the members get the mtime from it.
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
    // C++17:
    //
    // static_assert (atomic<timestamp::rep>::is_always_lock_free,
    //               "timestamp is not lock-free on this architecture");

#if !defined(ATOMIC_LLONG_LOCK_FREE) || ATOMIC_LLONG_LOCK_FREE != 2
#  error timestamp is not lock-free on this architecture
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
  class path_target: public mtime_target
  {
  public:
    using mtime_target::mtime_target;

    typedef build2::path path_type;

    // Target path is an "atomic consistent cash". That is, it can be set at
    // any time but any subsequent updates must set the same path. Or, in
    // other words, once the path is set, it never changes.
    //
    // A set empty path may signify special unknown/undetermined location (for
    // example an installed import library -- we know it's there, just not
    // exactly where). In this case you would also normally set its mtime. We
    // used to return a pointer to properly distinguish between not set and
    // empty but that proved too tedious. Note that this means there could be
    // a race between path and mtime (unless you lock the target in some other
    // way; see file_rule) so for this case it makes sense to set the
    // timestamp first.
    //
    const path_type&
    path () const;

    const path_type&
    path (path_type) const;

    timestamp
    load_mtime () const {return mtime_target::load_mtime (path ());}

    // Derive a path from target's dir, name, and, if set, ext. If ext is not
    // set, try to derive it using the target type extension function and
    // fallback to default_ext, if specified. In both cases also update the
    // target's extension (this becomes important if later we need to reliably
    // determine whether this file has an extension; think hxx{foo.bar.} and
    // hxx{*}:extension is empty).
    //
    // If name_prefix is not NULL, add it before the name part and after the
    // directory. Similarly, if name_suffix is not NULL, add it after the name
    // part and before the extension.
    //
    // Finally, if the path was already assigned to this target, then this
    // function verifies that the two are the same.
    //
    const path_type&
    derive_path (const char* default_ext = nullptr,
                 const char* name_prefix = nullptr,
                 const char* name_suffix = nullptr);

    // This version can be used to derive the path from another target's path
    // by adding another extension.
    //
    const path_type&
    derive_path (path_type base, const char* default_ext = nullptr);

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
  class file: public path_target
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
  class alias: public target
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
  class dir: public alias
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
  };

  // While a filesystem directory is mtime-based, the semantics is
  // not very useful in our case. In particular, if another target
  // depends on fsdir{}, then all that's desired is the creation of
  // the directory if it doesn't already exist. In particular, we
  // don't want to update the target just because some unrelated
  // entry was created in that directory.
  //
  class fsdir: public target
  {
  public:
    using target::target;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // Executable file.
  //
  class exe: public file
  {
  public:
    using file::file;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  class buildfile: public file
  {
  public:
    using file::file;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // This is the venerable .in ("input") file that needs some kind of
  // preprocessing.
  //
  // One interesting aspect of this target type is that the prerequisite
  // search is target-dependent. Consider:
  //
  // hxx{version}: in{version.hxx} // version.hxx.in -> version.hxx
  //
  // Having to specify the header extension explicitly is inelegant. Instead
  // what we really want to write is this:
  //
  // hxx{version}: in{version}
  //
  // But how do we know that in{version} means version.hxx.in? That's where
  // the target-dependent search comes in: we take into account the target
  // we are a prerequisite of.
  //
  class in: public file
  {
  public:
    using file::file;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // Common documentation file targets.
  //
  // @@ Maybe these should be in the built-in doc module?
  //
  class doc: public file
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
  class man: public doc
  {
  public:
    using doc::doc;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  class man1: public man
  {
  public:
    using man::man;

  public:
    static const target_type static_type;
    virtual const target_type& dynamic_type () const {return static_type;}
  };

  // Common implementation of the target factory, extension, and search
  // functions.
  //
  template <typename T>
  target*
  target_factory (const target_type&, dir_path d, dir_path o, string n)
  {
    return new T (move (d), move (o), move (n));
  }

  // Return fixed target extension unless one was specified.
  //
  template <const char* ext>
  const char*
  target_extension_fix (const target_key&);

  template <const char* ext>
  bool
  target_pattern_fix (const target_type&, const scope&, string&, bool);

  // Get the extension from the variable or use the default if none set. If
  // the default is NULL, then return NULL.
  //
  template <const char* var, const char* def>
  optional<string>
  target_extension_var (const target_key&, const scope&, bool);

  template <const char* var, const char* def>
  bool
  target_pattern_var (const target_type&, const scope&, string&, bool);

  // Always return NULL extension.
  //
  optional<string>
  target_extension_null (const target_key&, const scope&, bool);

  // Assert if called.
  //
  optional<string>
  target_extension_assert (const target_key&, const scope&, bool);

  // Target print functions.
  //

  // Target type uses the extension but it is fixed and there is no use
  // printing it (e.g., man1{}).
  //
  void
  target_print_0_ext_verb (ostream&, const target_key&);

  // Target type uses the extension and there is normally no default so it
  // should be printed (e.g., file{}).
  //
  void
  target_print_1_ext_verb (ostream&, const target_key&);

  // The default behavior, that is, look for an existing target in the
  // prerequisite's directory scope.
  //
  const target*
  target_search (const target&, const prerequisite_key&);

  // First look for an existing target as above. If not found, then look
  // for an existing file in the target-type-specific list of paths.
  //
  const target*
  file_search (const target&, const prerequisite_key&);
}

#include <build2/target.ixx>
#include <build2/target.txx>

#endif // BUILD2_TARGET_HXX
