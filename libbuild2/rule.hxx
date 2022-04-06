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
  // Note: match() is only called once but may not be followed by apply().
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
  struct match_extra;

  class LIBBUILD2_SYMEXPORT rule
  {
  public:
    virtual bool
    match (action, target&, const string& hint, match_extra&) const = 0;

    virtual recipe
    apply (action, target&, match_extra&) const = 0;

    rule () = default;

    virtual
    ~rule ();

    rule (const rule&) = delete;
    rule& operator= (const rule&) = delete;

    // Sometimes we want to match only if another rule of ours would match
    // another operation. For example, we would want our install rule to match
    // only if our update rule also matches.
    //
    // Arranging this, however, is not a simple matter of calling the other
    // rule's match(): we also have to take into account the rule hints for
    // that operation. This helper performs all the necessary steps. Note:
    // should only be called from match() (see target::find_hint() for
    // details).
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

    file_rule () {}

    static const file_rule instance;
    static const build2::rule_match rule_match;
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
    perform_update_direct (action, const target&);

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
  // the implementation cannot use the match_extra::buffer nor the target
  // auxiliary data storage until the pattern's apply_*() calls have been
  // made.
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
    virtual bool
    match (action, target&, const string&, match_extra&) const = 0;

    virtual void
    apply_adhoc_members (action, target&,
                         const scope& base,
                         match_extra&) const = 0;

    // The implementation should append pattern prerequisites to
    // t.prerequisite_targets[a] but not match. It should set bit 2 in
    // prerequisite_target::include to indicate update=match and bit 3
    // to indicate update=unmatch.
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
