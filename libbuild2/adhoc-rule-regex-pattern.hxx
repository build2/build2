// file      : libbuild2/adhoc-rule-regex-pattern.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_ADHOC_RULE_REGEX_PATTERN_HXX
#define LIBBUILD2_ADHOC_RULE_REGEX_PATTERN_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

namespace build2
{
  // Ad hoc rule regex pattern.
  //
  // The name signature string is stored in match_extra::data while the regex
  // match_results object -- in the target auxiliary data storage. Both must
  // remain valid until after the apply_*() calls.
  //
  // Note: exported since may be accessed by ad hoc recipe implementation.
  //
  class LIBBUILD2_SYMEXPORT adhoc_rule_regex_pattern: public adhoc_rule_pattern
  {
  public:
    using name  = build2::name;
    using scope = build2::scope;

    adhoc_rule_regex_pattern (const scope&, string, const target_type&,
                              name&&, const location&,
                              names&&, const location&,
                              names&&, const location&);

    virtual bool
    match (action, const target&, const string&, match_extra&) const override;

    virtual void
    apply_group_members (action, target&,
                         const scope&,
                         match_extra&) const override;

    virtual void
    apply_prerequisites (action, target&,
                         const scope&,
                         match_extra&) const override;

    virtual void
    dump (ostream&) const override;

  private:
    string text_;  // Pattern text.
    regex  regex_; // Pattern regex.

    struct element
    {
      build2::name       name;
      const target_type& type;
      bool               match_ext; // Match extension flag.
    };

    vector<element> targets_; // First is the primary target.
    vector<element> prereqs_;
  };
}

#endif // LIBBUILD2_ADHOC_RULE_REGEX_PATTERN_HXX
