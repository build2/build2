// file      : libbuild2/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TARGET_HXX
#define LIBBUILD2_TARGET_HXX

#include <cstddef>      // max_align_t
#include <iterator>     // tags, etc.
#include <type_traits>  // is_*
#include <unordered_map>

#include <libbutl/multi-index.hxx> // map_iterator_adapter

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
  // Note that posthoc is handled internally and should normally be treated by
  // the rules the same as excluded.
  //
  class include_type
  {
  public:
    enum value {excluded, posthoc, adhoc, normal};

    include_type (value v): v_ (v) {}
    include_type (bool  v): v_ (v ? normal : excluded) {}

    operator         value () const {return v_;}
    explicit operator bool () const {return v_ == normal || v_ == adhoc;}

  private:
    value v_;
  };

  // A view of target group members.
  //
  // Note that the members array may have "holes" (NULL pointers) and count is
  // the number of elements in this array (and not necessarily the number of
  // group members).
  //
  // Note that members being NULL and count being 0 have different meaning:
  // the former means group members are not known while the latter means it's
  // an empty group.
  //
  struct group_view
  {
    const target* const* members; // NULL means not yet known.
    size_t count;
  };

  // List of prerequisites resolved to targets. Unless additional storage is
  // needed, it can be treated as just vector<const target*> (which is what we
  // used to have initially).
  //
  // The include member normally just indicates (in the first bit) whether
  // this prerequisite is ad hoc. But it can also carry additional information
  // (for example, from operation-specific override) in other bits (see below
  // for details).
  //
  struct prerequisite_target
  {
    using target_type = build2::target;

    prerequisite_target (const target_type* t, bool a = false, uintptr_t d = 0)
        : target (t), include (a ? include_adhoc : 0), data (d) {}

    prerequisite_target (const target_type& t, bool a = false, uintptr_t d = 0)
        : prerequisite_target (&t, a, d) {}

    prerequisite_target (const target_type* t, include_type a, uintptr_t d = 0)
        : prerequisite_target (t, a == include_type::adhoc, d) {}

    prerequisite_target (const target_type& t, include_type a, uintptr_t d = 0)
        : prerequisite_target (&t, a, d) {}

    const target_type* target;

    operator const target_type*&  ()       {return target;}
    operator const target_type*   () const {return target;}
    const target_type* operator-> () const {return target;}

    // The first 8 bits are reserved with the first two having the following
    // semantics:
    //
    // adhoc
    //
    //   This prerequisite is ad hoc.
    //
    // udm
    //
    //   This prerequisite is updated during match. Note that only static
    //   prerequisites that are updated during match should have this bit set
    //   (see dyndep_rule::*_existing_file() for details).
    //
    // target
    //
    //   The data member contains the target pointer that has been "blanked
    //   out" for some reason (updated during match, unmatched, etc). See
    //   dyndep_rule::updated_during_match() for details.
    //
    static const uintptr_t include_adhoc  = 0x01;
    static const uintptr_t include_udm    = 0x02;
    static const uintptr_t include_target = 0x80;

    uintptr_t include;

    bool adhoc () const {return (include & include_adhoc) != 0;}

    // Auxiliary data.
    //
    uintptr_t data;
  };
  using prerequisite_targets = vector<prerequisite_target>;

  // A rule match is an element of name_rule_map.
  //
  using rule_match = pair<const string, reference_wrapper<const rule>>;

  // A map of target type plus operation ids to rule hints (see name_rule_map
  // for details on rule names and hints). The default_id serves as a fallback
  // for update and clean operations.
  //
  // Note that for now hints are tried in the order specified and the first
  // that matches, used.
  //
  struct rule_hints
  {
    // Return empty string if not found.
    //
    const string&
    find (const target_type&, operation_id, bool untyped) const;

    bool
    empty () const {return map.empty ();}

    // Note that insertion of an existing entry overrides the old value.
    //
    void
    insert (const target_type*, operation_id, string);

    struct value_type
    {
      const target_type* type;
      operation_id       operation;
      string             hint;
    };

    vector<value_type> map;
  };

  // Additional information about a rule match (see rule.hxx for details).
  //
  // Note that passing this information to a base rule's match() as-is may or
  // may not be correct. If some changes must be made (for example, the
  // fallback flag must be cleared), then that should be done by modifying
  // (and restoring, if necessary) the passed instance rather than making a
  // copy (which would not survive to apply()).
  //
  struct match_extra
  {
    bool locked;   // Normally true (see adhoc_rule::match() for background).
    bool fallback; // True if matching a fallback rule (see match_rule_impl()).

    // When matching a rule, the caller may wish to request a subset of the
    // full functionality of performing the operation on the target. This is
    // achieved with match options.
    //
    // Since the match caller normally has no control over which rule will be
    // matched, the options are not specific to a particular rule. Rather,
    // options are defined for performing a specific operation on a specific
    // target type and would normally be part of the target type semantics.
    // To put it another way, when a rule matches a target of certain type for
    // certain operation, there is an expectation of certain semantics, some
    // parts of which could be made optional.
    //
    // As a concrete example, consider installing libs{}, which traditionally
    // has two parts: runtime (normally just the versioned shared library) and
    // build-time (non-versioned symlinks, pkg-config files, headers, etc).
    // The option to install only the runtime files is part of the bin::libs{}
    // semantics, not of, say, cc::install_rule.
    //
    // The match options are specified as a uint64_t mask, which means there
    // can be a maximum of 64 options per operation/target type. Options are
    // opt-out rather than opt-in. That is, by default, all the options are
    // enabled unless the match caller explicitly opted out of some
    // functionality. Even if the caller opted out, there is no guarantee that
    // the matching rule will honor this request (for example, because it is a
    // user-provided ad hoc recipe). To put it another way, support for
    // options is a quality of implementation matter.
    //
    // From the rule implementation's point view, match options are handled as
    // follows: On initial match()/apply(), cur_options is initialized to ~0
    // (all options enabled) and the matching rule is expected to override it
    // with new_options in apply() (note that match() should no base any
    // decisions on new_options since they may change between match() and
    // apply()). This way a rule that does not support any match options does
    // not need to do anything. Subsequent match calls may add new options
    // which causes a rematch that manifests in the rule's reapply() call. In
    // reapply(), cur_options are the currently enabled options and
    // new_options are the newly requested options. Here the rule is expected
    // to factor new_options to cur_options as appropriate. Note also that on
    // rematch, if current options already include new options, then no call
    // to reapply() is made. This, in particular, means that a rule that does
    // not adjust cur_options in match() will never get a reapply() call
    // (because all the options are enabled from the start). Note that
    // cur_options should only be modfied in apply() or reapply().
    //
    // If a rematch is triggered after the rule has already been executed, an
    // error is issued.  This means that match options are not usable for
    // operation/target types that could plausibly be executed during
    // match. In particular, using match options for update and clean
    // operations is a bad idea (update of pretty much any target can happen
    // during match as a result of a tool update while clean might have to be
    // performed during match to provide the mirror semantics).
    //
    // Note also that with rematches the assumption that in the match phase
    // after matching the target we can MT-safely examine its state (such as
    // its prerequisite_targets) no longer holds since such state could be
    // modified during a rematch. As a result, if the target type specifies
    // options for a certain operation, then you should not rely on this
    // assumption for targets of this type during this operation.
    //
    // A rule that supports match options must also be prepared to handle the
    // apply() call with new_options set to 0, for example, by using a
    // minimally supported set of options instead. While 0 usually won't be
    // passed by the match caller, this value is passed in the following
    // circumstances:
    //
    //   - match to resolve group (resolve_group())
    //   - match to resolve members (resolve_members())
    //   - match of ad hoc group via one of its ad hoc members
    //
    // Note that the 0 cur_options value is illegal.
    //
    // When it comes to match options specified for group members, the
    // semantics differs between explicit and ad hoc groups. For explicit
    // groups, the standard semantics described above applies and the group's
    // reapply() function will be called both for the group itself as well as
    // for its members and its the responsibility of the rule to decide what
    // to do with the two sets of options (e.g., factor member's options into
    // group's options, etc). For ad hoc groups, members are not matched to a
    // rule but to the group_recipe directly (so there cannot be a call to
    // reapply()). Currently, ad hoc group members cannot have options (more
    // precisely, their options should always be ~0). An alternative semantics
    // where the group rule is called to translate member options to group
    // options may be implemented in the future (see match_impl_impl() for
    // details).
    //
    // Note: match options are currently not exposed in Buildscript ad hoc
    // recipes/rules (but are in C++).
    //
    static constexpr uint64_t all_options = ~uint64_t (0);

    uint64_t cur_options;
    uint64_t new_options;

    atomic<uint64_t> cur_options_; // Implementation detail (see lock_impl()).

    // The list of post hoc prerequisite targets for this target. Only not
    // NULL in rule::apply_posthoc() and rule::reapply() functions and only if
    // there are post hoc prerequisites. Primarily useful for adjusting match
    // options for post hoc prerequisites (but can also be used to blank some
    // of them out).
    //
    vector<context::posthoc_target::prerequisite_target>*
      posthoc_prerequisite_targets;

    // Auxiliary data storage.
    //
    // A rule (whether matches or not) may use this pad to pass data between
    // its match and apply functions (but not the recipe). The rule should
    // static assert that the size of the pad is sufficient for its needs.
    //
    // This facility is complementary to the auxiliary data storage in target:
    // it can store slightly more/extra data without dynamic memory allocation
    // but can only be used during match/apply.
    //
    // Note also that a rule that delegates to another rule may not be able to
    // use this mechanism fully since the delegated-to rule may also need the
    // data storage.
    //
    static constexpr size_t data_size = (sizeof (string) > sizeof (void*) * 4
                                         ? sizeof (string)
                                         : sizeof (void*) * 4);

    alignas (std::max_align_t) unsigned char data_[data_size];
    void (*data_dtor_) (void*) = nullptr;

    template <typename R,
              typename T = typename std::remove_cv<
                typename std::remove_reference<R>::type>::type>
    typename std::enable_if<std::is_trivially_destructible<T>::value,T&>::type
    data (R&& d)
    {
      assert (sizeof (T) <= data_size);
      clear_data ();
      return *new (&data_) T (forward<R> (d));
    }

    template <typename R,
              typename T = typename std::remove_cv<
                typename std::remove_reference<R>::type>::type>
    typename std::enable_if<!std::is_trivially_destructible<T>::value,T&>::type
    data (R&& d)
    {
      assert (sizeof (T) <= data_size);
      clear_data ();
      T& r (*new (&data_) T (forward<R> (d)));
      data_dtor_ = [] (void* p) {static_cast<T*> (p)->~T ();};
      return r;
    }

    template <typename T>
    T&
    data () {return *reinterpret_cast<T*> (&data_);}

    template <typename T>
    const T&
    data () const {return *reinterpret_cast<const T*> (&data_);}

    void
    clear_data ()
    {
      if (data_dtor_ != nullptr)
      {
        data_dtor_ (&data_);
        data_dtor_ = nullptr;
      }
    }

    // Implementation details.
    //
    // NOTE: see match_rule_impl() in algorithms.cxx if changing anything here.
    //
  public:
    explicit
    match_extra (bool l = true, bool f = false)
        : locked (l), fallback (f),
          cur_options (all_options), new_options (0),
          posthoc_prerequisite_targets (nullptr) {}

    void
    reinit (bool fallback);

    // Force freeing of the dynamically-allocated memory.
    //
    void
    free ();

    ~match_extra ()
    {
      clear_data ();
    }
  };

  // Target.
  //

  // A target can be entered for several reasons that are useful to
  // distinguish for diagnostics, when considering as the default
  // target, etc.
  //
  // Note that the order of the enumerators is arranged so that their
  // integral values indicate whether one "overrides" the other.
  //
  // We refer to the targets other than real and implied as
  // dynamically-created or just dynamic.
  //
  // @@ We have cases (like pkg-config extraction) where it should probably be
  //    prereq_file rather than implied (also audit targets.insert<> calls).
  //
  // @@ Also, synthesized dependency declarations (e.g., in cc::link_rule) are
  //    fuzzy: they feel more `real` than `implied`. Maybe introduce
  //    `synthesized` in-between?
  //
  // @@ There are also now dynamically-discovered targets (ad hoc group
  //    members; see depdb-dyndep --dyn-target) which currently end up
  //    with prereq_new.
  //
  enum class target_decl: uint8_t
  {
    prereq_new = 1, // Created from prerequisite (create_new_target()).
    prereq_file,    // Created from prerequisite/file (search_existing_file()).
    implied,        // Target-spec variable assignment, implicitly-entered, etc.
    real            // Real dependency declaration.
  };

  inline bool
  operator>= (target_decl l, target_decl r)
  {
    return static_cast<uint8_t> (l) >= static_cast<uint8_t> (r);
  }

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
    const string      name; // Empty for dir{} and fsdir{} targets.
    optional<string>* ext_; // Reference to value in target_key.

    const string* ext () const; // Return NULL if not specified.
    const string& ext (string);

    // As above but assume targets mutex is locked.
    //
    const string* ext_locked () const;

    const dir_path&
    out_dir () const {return out.empty () ? dir : out;}

    // Note that the target declaration should only be upgraded via the MT-
    // safe target_set::insert().
    //
    target_decl decl;

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
    // updated (and usually with a single command), though there could be some
    // members that are omitted, depending on the configuration (e.g., an
    // inline file not/being generated). When an all-group is mentioned as a
    // prerequisite, the rule is usually interested in the individual members
    // rather than the group target. For example, a C++ compile rule would
    // like to "see" the ?xx{} members when it gets a cli.cxx{} group.
    //
    // Which brings us to the group iteration mode. The target type contains a
    // flag called see_through that indicates whether the default iteration
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
    // member has been matched can be racy. However, once the member is linked
    // up to the group, this relationship is immutable. As a result, one can
    // atomically query the current value to see if already linked up (can be
    // used as an optimization, to avoid deadlocks, etc).
    //
    relaxed_atomic<const target*> group = nullptr;

    // What has been described above is an "explicit" group. That is, there is
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
    // So to support this requirement we have ad hoc groups. The idea is that
    // any target can be turned either by a user's declaration in a buildfile
    // or by the rule that matches it into an ad hoc group by chaining several
    // targets together.
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
    //   after the matching rule's apply() call.
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
    //   first member, this is normally what we want). But special semantics
    //   could be arranged; see var_backlink, for example.
    //
    // Note that ad hoc groups can be part of explicit groups. In a sense, we
    // have a two-level grouping: an explicit group with its members each of
    // which can be an ad hoc group. For example, lib{} contains libs{} which
    // may have an import stub as its ad hoc member.
    //
    // Use add_adhoc_member(), find_adhoc_member() from algorithms to manage
    // ad hoc members.
    //
    // One conceptual issue we have with our ad hoc group implementation is
    // that the behavior could be sensitive to the order in which the members
    // are specified (due to the primary member logic). For example, unless we
    // specify the header in the header/source group first, it will not be
    // installed. Perhaps the solution is to synthesize a separate group
    // target for the ad hoc members (with a special target type that rules
    // like install could recognize). See also the variable lookup semantics.
    // We could also probably support see_through via an attribute or some
    // such. Or perhaps such cases should be handled through explicit groups
    // and the ad hoc semantics is left to the non-see_through "primary
    // targets with a bunch of subordinates" cases. In other words, if the
    // members are "equal/symmetrical", then perhaps an explicit group is the
    // correct approach.
    //
    const_ptr<target> adhoc_member = nullptr;

    // Return true if this target is an ad hoc group (that is, its primary
    // member).
    //
    bool
    adhoc_group () const
    {
      // An ad hoc group can be a member of a normal group.
      //
      return adhoc_member != nullptr &&
        (group == nullptr || group->adhoc_member == nullptr);
    }

    // Return true if this target is an ad hoc group member (that is, its
    // secondary member).
    //
    bool
    adhoc_group_member () const
    {
      return group != nullptr && group->adhoc_member != nullptr;
    }

  public:
    // Normally you should not call this function directly and rather use
    // resolve_members() from <libbuild2/algorithm.hxx>. Note that action
    // is always inner.
    //
    virtual group_view
    group_members (action) const;

    // Note that the returned key "tracks" the target (except for the
    // extension).
    //
    target_key
    key () const;

    // As above but assume targets mutex is locked.
    //
    target_key
    key_locked () const;

    // Note that the returned name is guaranteed to be "stable" (e.g., for
    // hashing) only if the target has the extension assigned. This happens,
    // for example, when a path is derived for a path-based target (which
    // normally happens when such a target is matched for update).
    //
    names
    as_name () const;

    void
    as_name (names&) const;

    // Scoping.
    //
   public:
    // Most qualified scope that contains this target.
    //
    const scope&
    base_scope () const
    {
      if (ctx.phase != run_phase::load)
      {
        if (const scope* s = base_scope_.load (memory_order_consume))
          return *s;
      }

      return base_scope_impl ();
    }

    // Root scope of a project that contains this target. Note that
    // a target can be out of any (known) project root in which case
    // this function asserts. If you need to detect this situation,
    // then use base_scope().root_scope() expression instead.
    //
    const scope&
    root_scope () const
    {
      return *base_scope ().root_scope ();
    }

    // Root scope of a bundle amalgamation that contains this target. The
    // same notes as to root_scope() apply.
    //
    const scope&
    bundle_scope () const {return *root_scope ().bundle_scope ();}

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

    // Implementation details (use above functions instead).
    //
    // Base scope cached value. Invalidated every time we switch to the load
    // phase (which is the only phase where we may insert new scopes).
    //
    mutable atomic<const scope*> base_scope_ {nullptr};

    const scope&
    base_scope_impl () const;

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
    // someone beat us to it), in which case the passed prerequisites are
    // not moved. Note that it can be called on const target.
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
      const scope& bs (base_scope ());
      const variable* var (bs.var_pool ().find (name));
      return var != nullptr ? lookup (*var, &bs).first : lookup_type ();
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
    lookup (const variable& var, const scope* bs = nullptr) const
    {
      auto p (lookup_original (var, false, bs));
      return var.overrides == nullptr
        ? p
        : (bs != nullptr
           ? *bs
           : base_scope ()).lookup_override (var, move (p), true);
    }

    // If target_only is true, then only look in target and its target group
    // without continuing in scopes. As an optimization, the caller can also
    // pass the base scope of the target, if already known. If locked is true,
    // assume the targets mutex is locked.
    //
    pair<lookup_type, size_t>
    lookup_original (const variable&,
                     bool target_only = false,
                     const scope* bs = nullptr,
                     bool locked = false) const;

    // Return a value suitable for assignment. See scope for details.
    //
    value&
    assign (const variable& var) {return vars.assign (var);}

    value&
    assign (const variable* var) {return vars.assign (var);} // For cached.

    // Note: variable must already be entered.
    //
    value&
    assign (const string& name)
    {
      return vars.assign (base_scope ().var_pool ().find (name));
    }

    // Return a value suitable for appending. See scope for details.
    //
    value&
    append (const variable&, const scope* bs = nullptr);

    // Note: variable must already be entered.
    //
    value&
    append (const string& name)
    {
      const scope& bs (base_scope ());
      return append (*bs.var_pool ().find (name), &bs);
    }

    // As above but assume the targets mutex is locked.
    //
    value&
    append_locked (const variable&, const scope* bs = nullptr);

    // Note: variable must already be entered.
    //
    value&
    append_locked (const string& name)
    {
      const scope& bs (base_scope ());
      return append_locked (*bs.var_pool ().find (name), &bs);
    }

    // Rule hints.
    //
  public:
    build2::rule_hints rule_hints;

    // Find the rule hint for the specified operation taking into account the
    // target type/group. Note: racy with regards to the group link-up and
    // should only be called when safe.
    //
    const string&
    find_hint (operation_id) const;

    // Ad hoc recipes.
    //
  public:
    vector<shared_ptr<adhoc_rule>> adhoc_recipes;

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

    // @@ PERF There is a lot of data below that is only needed for "output"
    //    as opposed to "source" targets (data pads, prerequisite_targets,
    //    etc). Maybe we should move this stuff to an optional extra (like we
    //    have for the root scope). Maybe we could even allocate it as part of
    //    the target's memory block or some such?

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

      // Match state storage between the match() and apply() calls with only
      // the *_options members extended to reapply().
      //
      // Note: in reality, cur_options are used beyong (re)apply() as an
      // implementation detail.
      //
      build2::match_extra match_extra;

      // Matched rule (pointer to name_rule_map element). Note that in case of
      // a direct recipe assignment we may not have a rule (NULL).
      //
      const rule_match* rule;

      // Applied recipe.
      //
      // Note: also used as the auxiliary data storage during match, which is
      //       why mutable (see the target::data() API below for details). The
      //       default recipe_keep value is set by clear_target().
      //
      mutable build2::recipe recipe;
      mutable bool           recipe_keep;         // Keep after execution.
      bool                   recipe_group_action; // Recipe is group_action.

      // Target state for this operation. Note that it is undetermined until
      // a rule is matched and recipe applied (see set_recipe()).
      //
      target_state state;

      // Set to true (only for the inner action) if this target has been
      // matched but not executed as a result of the resolve_members() call.
      // See also context::resolve_count.
      //
      bool resolve_counted;

      // Rule-specific variables.
      //
      // The rule (for this action) has to be matched before these variables
      // can be accessed and only the rule being matched can modify them (so
      // no iffy modifications of the group's variables by member's rules).
      //
      // They are also automatically cleared before another rule is matched,
      // similar to the auxiliary data storage. In other words, rule-specific
      // variables are only valid for this match-execute phase.
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

      // Implementation details.
      //
    public:
      explicit
      opstate (context& c): vars (variable_map::owner::target, &c) {}

    private:
      friend class target_set;

      // Back-pointer, set by target_set along with vars.target_.
      //
      const target* target_ = nullptr;
    };

    action_state<opstate> state;

    opstate&       operator[] (action a)       {return state[a];}
    const opstate& operator[] (action a) const {return state[a];}

    // Return true if the target has been matched for the specified action.
    // This function can only be called during the match or execute phases.
    //
    // If you need to observe something in the matched target (e.g., the
    // matched rule or recipe), use memory_order_acquire.
    //
    bool
    matched (action, memory_order mo = memory_order_relaxed) const;

    // This function can only be called during match if we have observed
    // (synchronization-wise) that this target has been matched (i.e., the
    // rule has been applied) for this action.
    //
    target_state
    matched_state (action, bool fail = true) const;

    // See try_match_sync().
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
    // It can also be called during the serial load phase (but make sure you
    // understand what you are doing).
    //
    target_state
    executed_state (action, bool fail = true) const;

    // Return true if the state comes from the group. Target must be at least
    // matched except for ad hoc group members during the execute phase.
    //
    bool
    group_state (action) const;

  protected:
    // Version that should be used during match after the target has been
    // matched for this action.
    //
    // Indicate whether there is a rule match with the first half of the
    // result (see try_match_sync()).
    //
    pair<bool, target_state>
    matched_state_impl (action) const;

    // Return fail-untranslated (but group-translated) state assuming the
    // target is executed and synchronized.
    //
    target_state
    executed_state_impl (action) const;

  public:
    // Targets to which prerequisites resolve for this action. Note that
    // unlike prerequisite::target, these can be resolved to group members.
    // NULL means the target should be skipped (or the rule may simply not add
    // such a target to the list).
    //
    // A rule should make sure that the target's prerequisite_targets are in
    // the "canonical" form (that is, all the prerequisites that need to be
    // executed are present with prerequisite_target::target pointing to the
    // corresponding target). This is relied upon in a number of places,
    // including in dump and to be able to pretend-execute the operation on
    // this target without actually calling the recipe (see perform_execute(),
    // resolve_members_impl() for background). Note that a rule should not
    // store targets that are semantically prerequisites in an ad hoc manner
    // (e.g., in match data) with a few well-known execeptions (see
    // group_recipe and inner_recipe).
    //
    // Note that the recipe may modify this list during execute. Normally this
    // would be just blanking out of ad hoc prerequisites, in which case check
    // for ad hoc first and for not NULL second if accessing prerequisites of
    // targets that you did not execute (see the library metadata protocol in
    // cc for an example).
    //
    mutable action_state<build2::prerequisite_targets> prerequisite_targets;

    // Auxiliary data storage.
    //
    // A rule that matches (i.e., returns true from its match() function) may
    // use this facility to pass data between its match and apply functions as
    // well as the recipe. Specifically, between match() and apply() the data
    // is stored in the recipe member (which is std::move_only_function-like).
    // If the data needs to be passed on to the recipe, then it must become
    // the recipe itself. Here is a typical arrangement:
    //
    // class compile_rule
    // {
    //   struct match_data
    //   {
    //     ... // Data.
    //
    //     const compile_rule& rule;
    //
    //     target_state
    //     operator() (action a, const target& t)
    //     {
    //       return rule.perform_update (a, t, this);
    //     }
    //   };
    //
    //   virtual bool
    //   match (action a, const target& t)
    //   {
    //     ... // Determine if matching.
    //
    //     t.data (a, match_data {..., *this});
    //     return true;
    //   }
    //
    //   virtual bool
    //   apply (action a, target& t)
    //   {
    //     match_data& md (t.data (a));
    //
    //     ... // Match prerequisites, etc.
    //
    //     return move (md); // Data becomes the recipe.
    //   }
    //
    //   target_state
    //   perform_update (action a, const target& t, match_data& md) const
    //   {
    //     ... // Access data (also available as t.data<match_data> (a)).
    //   }
    // };
    //
    // Note: see also similar facility in match_extra.
    //
    // After the recipe is executed, the recipe/data is destroyed, unless
    // explicitly requested not to (see below). The rule may static assert
    // that the small size of the storage (which doesn't require dynamic
    // memory allocation) is sufficient for its needs.
    //
    // Note also that a rule that delegates to another rule may need to store
    // the base rule's data/recipe in its own data/recipe.

    // Provide the small object optimization size for the common compilers
    // (see recipe.hxx for details) in case a rule wants to make sure its data
    // won't require a dynamic memory allocation. Note that using a minimum
    // generally available (2 pointers) is not always possible because the
    // data size may depend on sizes of other compiler-specific types (e.g.,
    // std::string).
    //
    static constexpr size_t small_data_size =
#if defined(__GLIBCXX__)
      sizeof (void*) * 2
#elif defined(_LIBCPP_VERSION)
      sizeof (void*) * 3
#elif defined(_MSC_VER)
      sizeof (void*) * 6
#else
      sizeof (void*) * 2 // Assume at least 2 pointers.
#endif
      ;

    template <typename T>
    struct data_wrapper
    {
      T d;

      target_state
      operator() (action, const target&) const // Never called.
      {
        return target_state::unknown;
      }
    };

    // Avoid wrapping the data if it is already a recipe.
    //
    // Note that this techniques requires a fix for LWG issue 2132 (which all
    // our minimum supported compiler versions appear to have).
    //
    template <typename T>
    struct data_invocable: std::is_constructible<
      std::function<recipe_function>,
      std::reference_wrapper<typename std::remove_reference<T>::type>> {};

    template <typename T>
    typename std::enable_if<!data_invocable<T>::value, void>::type
    data (action a, T&& d) const
    {
      using V = typename std::remove_cv<
        typename std::remove_reference<T>::type>::type;

      const opstate& s (state[a]);
      s.recipe = data_wrapper<V> {forward<T> (d)};
      s.recipe_keep = false; // Can't keep non-recipe data.
    }

    template <typename T>
    typename std::enable_if<!data_invocable<T>::value, T&>::type
    data (action a) const
    {
      using V = typename std::remove_cv<T>::type;
      return state[a].recipe.target<data_wrapper<V>> ()->d;
    }

    // Return NULL if there is no data or the data is of a different type.
    //
    template <typename T>
    typename std::enable_if<!data_invocable<T>::value, T*>::type
    try_data (action a) const
    {
      using V = typename std::remove_cv<T>::type;

      if (auto& r = state[a].recipe)
        if (auto* t = r.target<data_wrapper<V>> ())
          return &t->d;

      return nullptr;
    }

    // Note that in this case we don't strip const (the expectation is that we
    // move the recipe in/out of data).
    //
    // If keep is true, then keep the recipe as data after execution. In
    // particular, this can be used to communicate between inner/outer rules
    // (see cc::install_rule for an example).
    //
    //
    template <typename T>
    typename std::enable_if<data_invocable<T>::value, void>::type
    data (action a, T&& d, bool keep = false) const
    {
      const opstate& s (state[a]);
      s.recipe = forward<T> (d);
      s.recipe_keep = keep;
    }

    void
    keep_data (action a, bool keep = true) const
    {
      state[a].recipe_keep = keep;
    }

    template <typename T>
    typename std::enable_if<data_invocable<T>::value, T&>::type
    data (action a) const
    {
      return *state[a].recipe.target<T> ();
    }

    template <typename T>
    typename std::enable_if<data_invocable<T>::value, T*>::type
    try_data (action a) const
    {
      auto& r = state[a].recipe;
      return r ? r.target<T> () : nullptr;
    }

    // Target type info and casting.
    //
  public:
    const target*
    is_a (const target_type& tt) const
    {
      return type ().is_a (tt) ? this : nullptr;
    }

    template <typename T>
    T*
    is_a ()
    {
      // At least with GCC we see slightly better and more consistent
      // performance with our own type information.
      //
#if 0
      return dynamic_cast<T*> (this);
#else
      // We can skip dynamically-derived type here (derived_type).
      //
      return dynamic_type->is_a<T> () ? static_cast<T*> (this) : nullptr;
#endif
    }

    template <typename T>
    const T*
    is_a () const
    {
#if 0
      return dynamic_cast<const T*> (this);
#else
      return dynamic_type->is_a<T> () ? static_cast<const T*> (this) : nullptr;
#endif
    }

    const target*
    is_a (const char* n) const
    {
      return type ().is_a (n) ? this : nullptr;
    }

    // Unchecked cast.
    //
    template <typename T>
    T&
    as () {return static_cast<T&> (*this);}

    template <typename T>
    const T&
    as () const {return static_cast<const T&> (*this);}

    // Target type information.
    //
    // A derived target is expected to set dynamic_type to its static_type in
    // its constructor body.
    //
    // We also have dynamic "derivation" support (e.g., via define in
    // buildfile).
    //
    const target_type&
    type () const
    {
      return derived_type != nullptr ? *derived_type : *dynamic_type;
    }

    static const target_type static_type;
    const target_type* dynamic_type;
    const target_type* derived_type = nullptr;

    // RW access.
    //
    target&
    rw () const
    {
      assert (ctx.phase == run_phase::load);
      return const_cast<target&> (*this);
    }

  public:
    // Split the name (not necessarily a simple path) into target name (in
    // place) and extension (returned).
    //
    static optional<string>
    split_name (string&, const location&);

    // Combine the target name (not necessarily a simple path) and
    // extension.
    //
    // If the target type has the default extension, then "escape" the
    // existing extension if any.
    //
    static void
    combine_name (string&, const optional<string>&, bool default_extension);

    // Targets should be created via the targets set below.
    //
  protected:
    friend class target_set;

    target (context& c, dir_path d, dir_path o, string n)
        : ctx (c),
          dir (move (d)), out (move (o)), name (move (n)),
          vars (*this, false /* shared */),
          state (c)
    {
      dynamic_type = &static_type;
    }

  public:
    target (target&&) = delete;
    target& operator= (target&&) = delete;

    target (const target&) = delete;
    target& operator= (const target&) = delete;

    virtual
    ~target ();
  };

  // All targets are from the targets set below.
  //
  inline bool
  operator== (const target& x, const target& y) {return &x == &y;}

  inline bool
  operator!= (const target& x, const target& y) {return !(x == y);}

  // Note that if the targets mutex is locked, then calling this operator
  // will lead to a deadlock. Instead, do:
  //
  // ... << t.key_locked () << ...
  //
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

  // Helper for dealing with the prerequisite inclusion/exclusion (see
  // var_include in context.hxx).
  //
  // If the lookup argument is not NULL, then it will be set to the operation-
  // specific override, if present. Note that in this case the caller is
  // expected to validate that the override value is valid (note: use the same
  // diagnostics as in include() for consistency).
  //
  // Note that the include(prerequisite_member) overload is also provided.
  //
  include_type
  include (action, const target&, const prerequisite&, lookup* = nullptr);

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

    // Return as a new prerequisite instance. Note that it includes a copy
    // of prerequisite-specific variables.
    //
    prerequisite_type
    as_prerequisite () const;
  };

  // It is often stored as the target's auxiliary data so make sure there is
  // no destructor overhead.
  //
  static_assert (std::is_trivially_destructible<prerequisite_member>::value,
                 "prerequisite_member is not trivially destructible");

  inline bool
  operator== (const prerequisite_member& x, const prerequisite_member& y)
  {
    return &x.prerequisite == &y.prerequisite && x.member == y.member;
  }

  inline bool
  operator!= (const prerequisite_member& x, const prerequisite_member& y)
  {
    return !(x == y);
  }

  inline ostream&
  operator<< (ostream& os, const prerequisite_member& pm)
  {
    return os << pm.key ();
  }

  include_type
  include (action, const target&, const prerequisite_member&, lookup* = nullptr);

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
  // Note that if the group is empty, then we see the group itself (rather
  // than nothing). Failed that, an empty group would never be executed (e.g.,
  // during clean) since there is no member to trigger the group execution.
  // Other than that, it feels like seeing the group in this cases should be
  // harmless (i.e., rules are generally prepared to see prerequisites they
  // don't recognize).
  //
  enum class members_mode
  {
    always, // Iterate over members if not empty, group if empty, assert if
            // not resolvable.
    maybe,  // Iterate over members if resolvable and not empty, group
            // otherwise.
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
            i_->type.see_through ())
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
      // information is not available (note: return true if the group is
      // empty). Similar to leave_group(), you should increment the iterator
      // after calling this function provided group() returns true (see
      // below).
      //
      bool
      enter_group ();

      // Return true if the next element is this group's members. Normally
      // used to iterate over group members only, for example:
      //
      // for (...; ++i)
      // {
      //   if (i->prerequisite.type.see_through ())
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
      alignas (value_type) mutable unsigned char m_[sizeof (value_type)];
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
  // @@ TODO: we currently do not detect ambiguity if there are multiple merge
  //    candidates for a no-extension key. We could probably do it using the
  //    unordered_map::bucket() API.
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
      slock l (mutex_, defer_lock); if (ctx.phase != run_phase::load) l.lock ();
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
    // since nobody can see it until the lock is released. Note that there
    // is normally quite a bit of contention around this map so make sure to
    // not hold the lock longer than absolutely necessary.
    //
    // If skip_find is true, then don't first try to find an existing target
    // with a shared lock, instead going directly for the unique lock and
    // insert. It's a good idea to pass true as this argument if you know the
    // target is unlikely to be there.
    //
    // If need_lock is false, then release the lock (the target insertion is
    // indicated by the presence of the associated mutex).
    //
    pair<target&, ulock>
    insert_locked (const target_type&,
                   dir_path dir,
                   dir_path out,
                   string name,
                   optional<string> ext,
                   target_decl,
                   tracer&,
                   bool skip_find = false,
                   bool need_lock = true);

    // As above but instead of the lock return an indication of whether the
    // target was inserted.
    //
    pair<target&, bool>
    insert (const target_type& tt,
            dir_path dir,
            dir_path out,
            string name,
            optional<string> ext,
            target_decl decl,
            tracer& t,
            bool skip_find = false)
    {
      auto p (insert_locked (tt,
                             move (dir),
                             move (out),
                             move (name),
                             move (ext),
                             decl,
                             t,
                             skip_find,
                             false));

      return pair<target&, bool> (p.first, p.second.mutex () != nullptr);
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
            tracer& t,
            bool skip_find = false)
    {
      return insert (tt,
                     move (dir),
                     move (out),
                     move (name),
                     move (ext),
                     target_decl::implied,
                     t,
                     skip_find).first.template as<T> ();
    }

    template <typename T>
    T&
    insert (const dir_path& dir,
            const dir_path& out,
            const string& name,
            const optional<string>& ext,
            tracer& t,
            bool skip_find = false)
    {
      return insert<T> (T::static_type, dir, out, name, ext, t, skip_find);
    }

    template <typename T>
    T&
    insert (const dir_path& dir,
            const dir_path& out,
            const string& name,
            tracer& t,
            bool skip_find = false)
    {
      return insert<T> (dir, out, name, nullopt, t, skip_find);
    }

    // Note: not MT-safe so can only be used during serial execution.
    //
  public:
    using iterator = butl::map_iterator_adapter<map_type::iterator>;
    using const_iterator = butl::map_iterator_adapter<map_type::const_iterator>;

    iterator begin () {return map_.begin ();}
    iterator end ()   {return map_.end ();}

    const_iterator begin () const {return map_.begin ();}
    const_iterator end ()   const {return map_.end ();}

    size_t
    size () const {return map_.size ();}

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

#if 0
    size_t buckets_ = 0;
#endif
  };

  // Modification time-based target.
  //
  class LIBBUILD2_SYMEXPORT mtime_target: public target
  {
  public:
    mtime_target (context& c, dir_path d, dir_path o, string n)
      : target (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

    // Modification time is an "atomic cache". That is, it can be set at any
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
    // NOTE: if setting both path and mtime (typically during match), then use
    // the path_target::path_mtime() function to do it in the correct order.
    //
    void
    mtime (timestamp) const;

    // If the mtime is unknown, then load it from the filesystem also caching
    // the result.
    //
    // Note: must not be used if the target state is group.
    //
    timestamp
    load_mtime (const path&) const;

    // Return true if this target is newer than the specified timestamp.
    //
    // Note: can only be called during execute on a synchronized target.
    //
    bool
    newer (timestamp) const;

    // As above but for cases where the state is already queried.
    //
    bool
    newer (timestamp, target_state) const;

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
    path_target (context& c, dir_path d, dir_path o, string n)
      : mtime_target (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

    using path_type = build2::path;

    // Target path. Must be absolute and normalized.
    //
    // Target path is an "atomic consistent cache". That is, it can be set at
    // any time (including on a const instance) but any subsequent updates
    // must set the same path. Or, in other words, once the path is set, it
    // never changes.
    //
    // An empty path may signify special unknown/undetermined/unreal location
    // (for example, a binless library or an installed import library -- we
    // know the DLL is there, just not exactly where). In this case you could
    // also set its mtime to timestamp_unreal (but don't have to, if a real
    // timestamp can be derived, for example, from the import library in the
    // DLL case above).
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
    // NOTE: if setting both path and mtime (typically during match), then use
    // the path_mtime() function to do it in the correct order.
    //
    const path_type&
    path (memory_order = memory_order_acquire) const;

    const path_type&
    path (path_type) const;

    // Set both path and mtime and in the correct order.
    //
    const path_type&
    path_mtime (path_type, timestamp) const;

    // Load mtime using the cached path.
    //
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

    // As above but with the already derived (by calling derive_extension())
    // extension.
    //
    const path_type&
    derive_path_with_extension (const string& ext,
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

    // As above but with the already derived (by calling derive_extension())
    // extension.
    //
    const path_type&
    derive_path_with_extension (path_type base,
                                const string& ext,
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
    file (context& c, dir_path d, dir_path o, string n)
      : path_target (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  // Mtime-based group target.
  //
  // Used to support explicit groups in buildfiles: can be derived from,
  // populated with static members using the group{foo}<...> syntax, and
  // matched with an ad hoc recipe/rule, including dynamic member extraction.
  // Note that it is not see-through but a derived group can be made see-
  // through via the [see_through] attribute.
  //
  // Note also that you shouldn't use it as a base for a custom group defined
  // in C++, instead deriving from mtime_target directly and using a custom
  // members layout more appropriate for the group's semantics. To put it
  // another way, a group-based target should only be matched by an ad hoc
  // recipe/rule (see match_rule_impl() in algorithms.cxx for details).
  //
  class LIBBUILD2_SYMEXPORT group: public mtime_target
  {
  public:
    vector<reference_wrapper<const target>> static_members;

    // Note: we expect no NULL entries in members.
    //
    vector<const target*> members; // Layout compatible with group_view.
    action members_action; // Action on which members were resolved.
    size_t members_on = 0; // Operation number on which members were resolved.
    size_t members_static; // Number of static ones in members (always first).

    void
    reset_members (action a)
    {
      members.clear ();
      members_action = a;
      members_on = ctx.current_on;
      members_static = 0;
    }

    virtual group_view
    group_members (action) const override;

    group (context& c, dir_path d, dir_path o, string n)
      : mtime_target (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  // Alias target. It represents a list of targets (its prerequisites)
  // as a single "name".
  //
  class LIBBUILD2_SYMEXPORT alias: public target
  {
  public:
    alias (context& c, dir_path d, dir_path o, string n)
      : target (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  // Directory target. Note that this is not a filesystem directory
  // but rather an alias target with the directory name. For actual
  // filesystem directory (creation), see fsdir.
  //
  class LIBBUILD2_SYMEXPORT dir: public alias
  {
  public:
    dir (context& c, dir_path d, dir_path o, string n)
      : alias (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;

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
    fsdir (context& c, dir_path d, dir_path o, string n)
      : target (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  // Executable file (not necessarily binary, though we do fallback to the
  // host machine executable extension in certain cases; see the default
  // extension derivation for details).
  //
  class LIBBUILD2_SYMEXPORT exe: public file
  {
  public:
    exe (context& c, dir_path d, dir_path o, string n)
      : file (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

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

    // Lookup metadata variable (see {import,export}.metadata).
    //
    template <typename T>
    const T*
    lookup_metadata (const char* var) const;

  public:
    static const target_type static_type;

  private:
    process_path_type process_path_;
  };

  class LIBBUILD2_SYMEXPORT buildfile: public file
  {
  public:
    buildfile (context& c, dir_path d, dir_path o, string n)
      : file (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  // This target type is primarily used for files mentioned in the `recipe`
  // directive.
  //
  class LIBBUILD2_SYMEXPORT buildscript: public file
  {
  public:
    buildscript (context& c, dir_path d, dir_path o, string n)
      : file (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  // Common documentation file target.
  //
  class LIBBUILD2_SYMEXPORT doc: public file
  {
  public:
    doc (context& c, dir_path d, dir_path o, string n)
      : file (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  // Legal files (LICENSE, AUTHORS, COPYRIGHT, etc).
  //
  class LIBBUILD2_SYMEXPORT legal: public doc
  {
  public:
    legal (context& c, dir_path d, dir_path o, string n)
      : doc (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
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
  // simply specify the extension explicitly, e.g., man1{foo.1p}.
  //
  class LIBBUILD2_SYMEXPORT man: public doc
  {
  public:
    man (context& c, dir_path d, dir_path o, string n)
      : doc (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  class LIBBUILD2_SYMEXPORT man1: public man
  {
  public:
    man1 (context& c, dir_path d, dir_path o, string n)
      : man (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
  };

  // We derive manifest from doc rather than file so that it get automatically
  // installed into the same place where the rest of the documentation goes.
  // If you think about it, it's kind of a documentation, similar to (but
  // better than) the version file that many projects come with.
  //
  class LIBBUILD2_SYMEXPORT manifest: public doc
  {
  public:
    manifest (context& c, dir_path d, dir_path o, string n)
      : doc (c, move (d), move (o), move (n))
    {
      dynamic_type = &static_type;
    }

  public:
    static const target_type static_type;
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

  LIBBUILD2_SYMEXPORT const char*
  target_extension_none (const target_key&, const scope*);

  LIBBUILD2_SYMEXPORT const char*
  target_extension_must (const target_key&, const scope*);

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

  // Target print functions (target_type::print).
  //

  // Target type uses the extension but it is fixed and there is no use
  // printing it (e.g., man1{}).
  //
  LIBBUILD2_SYMEXPORT bool
  target_print_0_ext_verb (ostream&, const target_key&, bool);

  // Target type uses the extension and there is normally no default so it
  // should be printed (e.g., file{}).
  //
  LIBBUILD2_SYMEXPORT bool
  target_print_1_ext_verb (ostream&, const target_key&, bool);

  // Target search functions (target_type::search).
  //

  // The default behavior, that is, look for an existing target in the
  // prerequisite's directory scope.
  //
  // Note that this implementation assumes a target can only be found in the
  // out tree (targets that can be in the src tree would normally use
  // file_search() below).
  //
  LIBBUILD2_SYMEXPORT const target*
  target_search (context&, const target*, const prerequisite_key&);

  // First look for an existing target both in out and src. If not found, then
  // look for an existing file in src.
  //
  LIBBUILD2_SYMEXPORT const target*
  file_search (context&, const target*, const prerequisite_key&);
}

#include <libbuild2/target.ixx>
#include <libbuild2/target.txx>

#endif // LIBBUILD2_TARGET_HXX
