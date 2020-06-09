// file      : libbuild2/rule-adhoc-buildscript.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_RULE_ADHOC_BUILDSCRIPT_HXX
#define LIBBUILD2_RULE_ADHOC_BUILDSCRIPT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/build/script/script.hxx>

namespace build2
{
  // Ad hoc buildscript rule.
  //
  // Note: not exported and should not be used directly (i.e., registered).
  //
  class adhoc_buildscript_rule: public adhoc_rule
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

    adhoc_buildscript_rule (const location& l, size_t b)
        : adhoc_rule ("<ad hoc buildscript recipe>", l, b) {}

    virtual bool
    recipe_text (context&, const target&, string&&, attributes&) override;

    virtual void
    dump_attributes (ostream&) const override;

    virtual void
    dump_text (ostream&, string&) const override;

  public:
    using script_type = build::script::script;

    script_type script;
    string      checksum; // Script text hash.
  };
}

#endif // LIBBUILD2_RULE_ADHOC_BUILDSCRIPT_HXX
