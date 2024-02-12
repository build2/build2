// file      : libbuild2/adhoc-rule-cxx.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_ADHOC_RULE_CXX_HXX
#define LIBBUILD2_ADHOC_RULE_CXX_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Ad hoc C++ rule.
  //
  // Note: exported but should not be used directly (i.e., registered).
  //
  class LIBBUILD2_SYMEXPORT cxx_rule: public simple_rule
  {
    // For now this class is provided purely as an alias for rule in case the
    // implementation (which is also called rule) needs to refer to something
    // in its base.
  };

  // Note that when used as part of a pattern, the implementation cannot use
  // the match_extra::data() facility nor the target auxiliary data storage.
  //
  class LIBBUILD2_SYMEXPORT cxx_rule_v1: public cxx_rule
  {
  public:
    // A robust recipe may want to incorporate the recipe_state into its
    // up-to-date decision as if the recipe library was a prerequisite (it
    // cannot be injected as a real prerequisite since it's from a different
    // build context).
    //
    const location     recipe_loc;     // Buildfile location of the recipe.
    const target_state recipe_state;   // State of recipe library target.
    const adhoc_rule_pattern* pattern; // Ad hoc pattern rule of recipe.

    cxx_rule_v1 (const location& l,
                 target_state s,
                 const adhoc_rule_pattern* p)
      : recipe_loc (l), recipe_state (s), pattern (p) {}

    // Return true by default.
    //
    // Note: must treat target as const (unless known to match a non-group).
    // See adhoc_rule::match() for background.
    //
    virtual bool
    match (action, target&) const override;

    using simple_rule::match; // Unhide the match_extra version.

    // Either this version or the one with match_extra must be overridden.
    //
    // If the pattern member above is not NULL then this recipe belongs to an
    // ad hoc pattern rule and the implementation may need to call the
    // pattern's apply_*() functions if the pattern has any ad hoc group
    // member substitutions or prerequisite substitutions/non-patterns,
    // respectively.
    //
    virtual recipe
    apply (action, target&) const override;

    using simple_rule::apply; // Unhide the match_extra version.
  };

  // Note: not exported.
  //
  class adhoc_cxx_rule: public adhoc_rule
  {
  public:
    virtual bool
    match (action, target&, const string&, match_extra&) const override;

    virtual recipe
    apply (action, target&, match_extra&) const override;

    virtual void
    reapply (action, target&, match_extra&) const override;

    adhoc_cxx_rule (string, const location&, size_t,
                    uint64_t ver,
                    optional<string> sep);

    virtual bool
    recipe_text (const scope&,
                 const target_type&,
                 string&&,
                 attributes&) override;

    virtual
    ~adhoc_cxx_rule () override;

    virtual void
    dump_text (ostream&, string&) const override;

  public:
    // Note that this recipe (rule instance) can be shared between multiple
    // targets which could all be matched in parallel.
    //
    uint64_t                  version;
    optional<string>          separator;
    string                    code;
    mutable atomic<cxx_rule*> impl;
  };
}

#endif // LIBBUILD2_ADHOC_RULE_CXX_HXX
