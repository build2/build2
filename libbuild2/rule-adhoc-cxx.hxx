// file      : libbuild2/rule-adhoc-cxx.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_RULE_ADHOC_CXX_HXX
#define LIBBUILD2_RULE_ADHOC_CXX_HXX

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

    adhoc_cxx_rule (const location&, size_t,
                    uint64_t ver,
                    optional<string> sep);

    virtual bool
    recipe_text (context&, const target&, string&& t, attributes&) override;

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

#endif // LIBBUILD2_RULE_ADHOC_CXX_HXX
