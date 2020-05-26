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

#include <libbuild2/build/script/script.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Once a rule is registered (for a scope), it is treated as immutable. If
  // you need to modify some state (e.g., counters or some such), then make
  // sure it is MT-safe.
  //
  // Note: match() is only called once but may not be followed by apply().
  //
  class LIBBUILD2_SYMEXPORT rule
  {
  public:
    virtual bool
    match (action, target&, const string& hint) const = 0;

    virtual recipe
    apply (action, target&) const = 0;

    rule () = default;

    virtual
    ~rule ();

    rule (const rule&) = delete;
    rule& operator= (const rule&) = delete;
  };

  // Fallback rule that only matches if the file exists. It will also match
  // an mtime_target provided it has a set timestamp.
  //
  class LIBBUILD2_SYMEXPORT file_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    file_rule () {}
    static const file_rule instance;
  };

  class LIBBUILD2_SYMEXPORT alias_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    alias_rule () {}
    static const alias_rule instance;
  };

  // Note that this rule ignores the dry_run flag; see mkdir() in filesystem
  // for the rationale.
  //
  class LIBBUILD2_SYMEXPORT fsdir_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

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
  class LIBBUILD2_SYMEXPORT noop_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    noop_rule () {}
    static const noop_rule instance;
  };

  // Ad hoc rule.
  //
  // Note: not exported.
  //
  class adhoc_rule: public rule
  {
  public:
    location_value loc;     // Buildfile location of the recipe.
    size_t         braces;  // Number of braces in multi-brace tokens.

    adhoc_rule (const location& l, size_t b)
      : loc (l),
        braces (b),
        rule_match ("adhoc", static_cast<const rule&> (*this)) {}

    // Set the rule text, handle any recipe-specific attributes, and return
    // true if the recipe builds anything in the build/recipes/ directory and
    // therefore requires cleanup.
    //
    virtual bool
    recipe_text (context&, const target&, string&&, attributes&) = 0;

  public:
    // Some of the operations come in compensating pairs, such as update and
    // clean, install and uninstall. An ad hoc rule implementation may choose
    // to provide a fallback implementation of a compensating operation if it
    // is providing the other half (passed in the fallback argument).
    //
    // The default implementation calls rule::match() if fallback is absent
    // and returns false if fallback is present. So an implementation that
    // doesn't care about this semantics can implement the straight rule
    // interface.
    //
    virtual bool
    match (action, target&, const string&, optional<action> fallback) const;

    virtual bool
    match (action, target&, const string&) const override;

    virtual void
    dump (ostream&, string& indentation) const = 0;

    // Implementation details.
    //
  public:
    build2::rule_match rule_match;

    static const dir_path recipes_build_dir;

    // Scope operation callback that cleans up ad hoc recipe builds.
    //
    static target_state
    clean_recipes_build (action, const scope&, const dir&);
  };

  // Ad hoc script rule.
  //
  // Note: not exported and should not be used directly (i.e., registered).
  //
  class adhoc_script_rule: public adhoc_rule
  {
  public:
    virtual bool
    match (action, target&, const string&, optional<action>) const override;

    virtual recipe
    apply (action, target&) const override;

    target_state
    perform_update_file (action, const target&) const;

    target_state
    default_action (action, const target&) const;

    virtual void
    dump (ostream&, string&) const override;

    adhoc_script_rule (const location& l, size_t b): adhoc_rule (l, b) {}

    virtual bool
    recipe_text (context&, const target&, string&&, attributes&) override;

  public:
    using script_type = build::script::script;

    script_type script;
    string      checksum; // Script text hash.
  };

  // Ad hoc C++ rule.
  //
  // Note: exported but should not be used directly (i.e., registered).
  //
  class LIBBUILD2_SYMEXPORT cxx_rule: public rule
  {
    // For now this class is provided purely as an alias for rule in case the
    // implementation (which is also called rule) needs to refer to something
    // in its base.
  };

  class LIBBUILD2_SYMEXPORT cxx_rule_v1: public cxx_rule
  {
  public:
    // A robust recipe may want to incorporate the recipe_state into its
    // up-to-date decision as if the recipe library was a prerequisite (it
    // cannot be injected as a real prerequisite since it's from a different
    // build context).
    //
    const location     recipe_loc;   // Buildfile location of the recipe.
    const target_state recipe_state; // State of recipe library target.

    cxx_rule_v1 (const location& l, target_state s)
      : recipe_loc (l), recipe_state (s) {}

    // Return true by default.
    //
    virtual bool
    match (action, target&, const string&) const override;
  };

  // Note: not exported.
  //
  class adhoc_cxx_rule: public adhoc_rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    virtual void
    dump (ostream&, string&) const override;

    adhoc_cxx_rule (const location&, size_t, uint64_t version);

    virtual bool
    recipe_text (context&, const target&, string&& t, attributes&) override;

    virtual
    ~adhoc_cxx_rule () override;

  public:
    // Note that this recipe (rule instance) can be shared between multiple
    // targets which could all be matched in parallel.
    //
    uint64_t                  version;
    string                    code;
    mutable atomic<cxx_rule*> impl;
  };
}

#endif // LIBBUILD2_RULE_HXX
