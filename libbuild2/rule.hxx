// file      : libbuild2/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_RULE_HXX
#define LIBBUILD2_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/recipe.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Rule interface (see also simple_rule below for a simplified version).
  //
  // Once a rule is registered (for a scope), it is treated as immutable. If
  // you need to modify some state (e.g., counters or some such), then make
  // sure things are MT-safe.
  //
  // Note: match() could be called multiple times (so should be idempotent)
  // and it may not be followed by apply().
  //
  // The hint argument is the rule hint, if any, that was used to select this
  // rule. While normally not factored into the match decision, a rule may
  // "try harder" if a hint was specified (see cc::link_rule for an example).
  //
  // The match_extra argument (the type is defined in target.hxx) is used to
  // pass additional information that is only needed by some rule
  // implementations. It is also a way for us to later pass more information
  // without breaking source compatibility.
  //
  // A rule may adjust post hoc prerequisites by overriding apply_posthoc().
  // See match_extra::posthoc_prerequisite_targets for background and details.
  //
  // A rule may support match options and if such a rule is rematched with
  // different options, then reapply() is called. See
  // match_extra::{cur,new}_options for background and details.
  //
  struct match_extra;

  class LIBBUILD2_SYMEXPORT rule
  {
  public:
    virtual bool
    match (action, target&, const string& hint, match_extra&) const = 0;

    virtual recipe
    apply (action, target&, match_extra&) const = 0;

    virtual void
    apply_posthoc (action, target&, match_extra&) const;

    virtual void
    reapply (action, target&, match_extra&) const;

    rule () = default;

    virtual
    ~rule ();

    rule (const rule&) = delete;
    rule& operator= (const rule&) = delete;

    // Resolve a project-qualified target in a rule-specific manner.
    //
    // This is optional functionality that may be provided by some rules to
    // facilitate immediate importation of certain target types. See the
    // import machinery for details. The default implementation always returns
    // NULL.
    //
    // Note that if this function returns a target, it should have the
    // extension assigned so that as_name() returns a stable name.
    //
    virtual const target*
    import (const prerequisite_key&,
            const optional<string>& metadata,
            const location&) const;

    // Sometimes we want to match only if another rule of ours would match
    // another operation. For example, we would want our install rule to match
    // only if our update rule also matches.
    //
    // Arranging this, however, is not a simple matter of calling the other
    // rule's match(): we also have to take into account ad hoc recipes and
    // rule hints for that operation. This helper performs all the necessary
    // checks. Note: should only be called from match() (see
    // target::find_hint() for details). Note also that ad hoc recipes are
    // checked for hint_op, not action's operation.
    //
    bool
    sub_match (const string& rule_name, operation_id hint_op,
               action, target&, match_extra&) const;
  };

  // Simplified interface for rules that don't care about the hint or extras.
  //
  class LIBBUILD2_SYMEXPORT simple_rule: public rule
  {
  public:
    virtual bool
    match (action, target&) const = 0;

    virtual recipe
    apply (action, target&) const = 0;

    virtual bool
    match (action, target&, const string&, match_extra&) const override;

    virtual recipe
    apply (action, target&, match_extra&) const override;

    // The simplified version of sub_match() above.
    //
    // Note that it calls the simplified match() directly rather than going
    // through the original.
    //
    bool
    sub_match (const string& rule_name, operation_id hint_op,
               action, target&) const;
  };

  // Fallback rule that only matches if the file exists. It will also match
  // an mtime_target provided it has a set timestamp.
  //
  // Note: this rule is "hot" because it matches every static source file and
  // so we don't use simple_rule to avoid two extra virtual calls.
  //
  class LIBBUILD2_SYMEXPORT file_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&, match_extra&) const override;

    virtual recipe
    apply (action, target&, match_extra&) const override;

    // While this rule expects an mtime_target-based target, sometimes it's
    // necessary to register it for something less specific (normally target)
    // in order to achieve the desired rule matching priority (see the dist
    // and config modules for an example). For such cases this rule can be
    // instructed to check the type and only match if it's mtime_target-based.
    //
    file_rule (bool match_type = false): match_type_ (match_type) {}

    static const file_rule instance; // Note: does not match the target type.
    static const build2::rule_match rule_match;

  private:
    bool match_type_;
  };

  class LIBBUILD2_SYMEXPORT alias_rule: public simple_rule
  {
  public:
    virtual bool
    match (action, target&) const override;

    virtual recipe
    apply (action, target&) const override;

    alias_rule () {}
    static const alias_rule instance;
  };

  // Note that this rule ignores the dry_run flag; see mkdir() in filesystem
  // for the rationale.
  //
  class LIBBUILD2_SYMEXPORT fsdir_rule: public simple_rule
  {
  public:
    virtual bool
    match (action, target&) const override;

    virtual recipe
    apply (action, target&) const override;

    static target_state
    perform_update (action, const target&);

    static target_state
    perform_clean (action, const target&);

    // Sometimes, as an optimization, we want to emulate execute_direct()
    // of fsdir{} without the overhead of switching to the execute phase.
    //
    static void
    perform_update_direct (action, const fsdir&);

    static void
    perform_clean_direct (action, const fsdir&);

    fsdir_rule () {}
    static const fsdir_rule instance;
  };

  // Fallback rule that always matches and does nothing.
  //
  class LIBBUILD2_SYMEXPORT noop_rule: public simple_rule
  {
  public:
    virtual bool
    match (action, target&) const override;

    virtual recipe
    apply (action, target&) const override;

    noop_rule () {}
    static const noop_rule instance;
  };

  // Ad hoc rule.
  //
  // Used for both ad hoc pattern rules and ad hoc recipes. For recipes, it's
  // essentially a rule of one case. Note that when used as part of a pattern,
  // the implementation cannot use the match_extra::data() facility nor the
  // target auxiliary data storage until the pattern's apply_*() calls have
  // been made.
  //
  // Note also that when used as part of a pattern, the rule is also register
  // for the dist meta-operation (unless there is an explicit recipe for dist)
  // in order to inject additional pattern prerequisites which may "pull"
  // additional sources into the distribution.
  //
  // Note: not exported.
  //
  class adhoc_rule_pattern;

  class adhoc_rule: public rule
  {
  public:
    location_value          loc;     // Buildfile location of the recipe.
    size_t                  braces;  // Number of braces in multi-brace tokens.
    small_vector<action, 1> actions; // Actions this rule is for.

    // If not NULL then this rule (recipe, really) belongs to an ad hoc
    // pattern rule and match() should call the pattern's match() and
    // apply() should call the pattern's apply_*() functions (see below).
    //
    const adhoc_rule_pattern* pattern = nullptr;

    adhoc_rule (string name, const location& l, size_t b)
      : loc (l),
        braces (b),
        rule_match (move (name), static_cast<const rule&> (*this)) {}

    // Set the rule text, handle any recipe-specific attributes, and return
    // true if the recipe builds anything in the build/recipes/ directory and
    // therefore requires cleanup.
    //
    // Scope is the scope of the recipe and target type is the type of the
    // first target (for ad hoc recipe) or primary group member type (for ad
    // hoc pattern rule). The idea is that an implementation may make certain
    // assumptions based on the first target type (e.g., file vs non-file
    // based) in which case it should also enforce (e.g., in match()) that any
    // other targets that share this recipe are also of suitable type.
    //
    // Note also that this function is called after the actions member has
    // been populated.
    //
    virtual bool
    recipe_text (const scope&, const target_type&, string&&, attributes&) = 0;

  public:
    // Some of the operations come in compensating pairs, such as update and
    // clean, install and uninstall. An ad hoc rule implementation may choose
    // to provide a fallback implementation of a reverse operation if it is
    // providing the other half.
    //
    virtual bool
    reverse_fallback (action, const target_type&) const;

    // The default implementation forwards to the pattern's match() if there
    // is a pattern and returns true otherwise.
    //
    // Note also that in case of a member of a group-based target, match() is
    // called on the group while apply() on the member (see match_rule_impl()
    // in algorithms.cxx for details). This means that match() may be called
    // without having the target locked and as a result match() should (unless
    // known to only match a non-group) treat the target as const and only
    // rely on immutable information (type, name, etc) since the group could
    // be matched concurrenly. This case can be detected by examining
    // match_extra::locked (see adhoc_rule_regex_pattern::match() for a
    // use-case).
    //
    virtual bool
    match (action, target&, const string&, match_extra&) const override;

    // Dump support.
    //
    virtual void
    dump_attributes (ostream&) const;

    virtual void
    dump_text (ostream&, string& indentation) const = 0;

    // Implementation details.
    //
  public:
    // The name in rule_match is used to match hints and in diagnostics. The
    // former does not apply to ad hoc recipes (but does apply to ad hoc
    // rules).
    //
    const build2::rule_match rule_match;

    static const dir_path recipes_build_dir;

    // Scope operation callback that cleans up ad hoc recipe builds.
    //
    static target_state
    clean_recipes_build (action, const scope&, const dir&);
  };

  // A mix-in interface for ad hoc rules that support recipes with deadlines.
  //
  class adhoc_rule_with_deadline
  {
  public:
    virtual
    ~adhoc_rule_with_deadline ();

    // Return empty recipe if one with the deadline cannot be provided for
    // this action. In this case the caller may fallback to the normal
    // apply().
    //
    virtual recipe
    apply (action, target&, match_extra&, const optional<timestamp>&) const = 0;
  };

  // Ad hoc rule pattern.
  //
  // Note: exported since may be accessed by ad hoc recipe implementation.
  //
  class LIBBUILD2_SYMEXPORT adhoc_rule_pattern
  {
  public:
    const scope&                            rule_scope;
    const string                            rule_name;
    const target_type&                      type;      // Primary target type.
    small_vector<shared_ptr<adhoc_rule>, 1> rules;     // Really a unique_ptr.

    adhoc_rule_pattern (const scope& s, string n, const target_type& t)
        : rule_scope (s),
          rule_name (move (n)),
          type (t),
          fallback_rule_ (rules) {}

    virtual
    ~adhoc_rule_pattern ();

  public:
    // Note: the adhoc_rule::match() restrictions apply here as well.
    //
    virtual bool
    match (action, const target&, const string&, match_extra&) const = 0;

    // Append additional group members. Note that this function should handle
    // both ad hoc and explicit groups.
    //
    virtual void
    apply_group_members (action, target&,
                         const scope& base,
                         match_extra&) const = 0;

    // The implementation should append pattern prerequisites to
    // t.prerequisite_targets[a] but not match. It should set bit 2 in
    // prerequisite_target::include to indicate update=match and bit 3
    // to indicate update=unmatch. It should also avoid adding duplicate
    // fsdir{} similar to the search_prerequisite*() functions.
    //
    virtual void
    apply_prerequisites (action, target&,
                         const scope& base,
                         match_extra&) const = 0;

    // Dump support.
    //
    virtual void
    dump (ostream&) const = 0;

    // Gory implementation details (see match_impl()).
    //
  public:
    class fallback_rule: public rule
    {
    public:
      const small_vector<shared_ptr<adhoc_rule>, 1>& rules;

      explicit
      fallback_rule (const small_vector<shared_ptr<adhoc_rule>, 1>& rs)
          : rules (rs) {}

      // Dummy (never called).
      //
      virtual bool
      match (action, target&, const string&, match_extra&) const override;

      virtual recipe
      apply (action, target&, match_extra&) const override;
    };

    fallback_rule fallback_rule_;
  };
}

#endif // LIBBUILD2_RULE_HXX
