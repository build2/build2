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
  // Once a rule is registered (for a scope), it is treated as immutable. If
  // you need to modify some state (e.g., counters or some such), then make
  // sure it is MT-safe.
  //
  // Note: match() is only called once but may not be followed by apply().
  //
  class rule
  {
  public:
    virtual bool
    match (action, target&, const string& hint) const = 0;

    virtual recipe
    apply (action, target&) const = 0;

    rule () = default;

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

  // Ad hoc recipe rule.
  //
  // Note: should not be used directly (e.g., registered, etc).
  //
  class LIBBUILD2_SYMEXPORT adhoc_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    static target_state
    perform_update (action, const target&, const adhoc_recipe&);

    adhoc_rule () {}
    static const adhoc_rule instance;
    static const rule_match match_instance;
  };
}

#endif // LIBBUILD2_RULE_HXX
