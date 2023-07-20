// file      : libbuild2/adhoc-rule-buildscript.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_ADHOC_RULE_BUILDSCRIPT_HXX
#define LIBBUILD2_ADHOC_RULE_BUILDSCRIPT_HXX

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
  class adhoc_buildscript_rule: public adhoc_rule,
                                public adhoc_rule_with_deadline
  {
  public:
    virtual bool
    reverse_fallback (action, const target_type&) const override;

    virtual bool
    match (action, target&, const string&, match_extra&) const override;

    virtual recipe
    apply (action, target&, match_extra&) const override;

    virtual recipe
    apply (action, target&, match_extra&,
           const optional<timestamp>&) const override;

    target_state
    perform_update_file_or_group (action, const target&) const;

    struct match_data;
    struct match_data_byproduct;

    target_state
    perform_update_file_or_group_dyndep (
      action, const target&, match_data&) const;

    target_state
    perform_update_file_or_group_dyndep_byproduct (
      action, const target&, match_data_byproduct&) const;

    optional<target_state>
    execute_update_prerequisites (action, const target&, timestamp) const;

    bool
    execute_update_file (const scope&,
                         action a, const file&,
                         build::script::environment&,
                         build::script::default_runner&,
                         bool deferred_failure = false) const;

    bool
    execute_update_group (const scope&,
                          action a, const group&,
                          build::script::environment&,
                          build::script::default_runner&,
                          bool deferred_failure = false) const;

    static target_state
    perform_clean_file (action, const target&);

    static target_state
    perform_clean_group (action, const target&);

    target_state
    default_action (action, const target&, const optional<timestamp>&) const;

    adhoc_buildscript_rule (string n, const location& l, size_t b)
        : adhoc_rule (move (n), l, b) {}

    virtual bool
    recipe_text (const scope&,
                 const target_type&,
                 string&&,
                 attributes&) override;

    virtual void
    dump_attributes (ostream&) const override;

    virtual void
    dump_text (ostream&, string&) const override;

    void
    print_custom_diag (const scope&, names&&, const location&) const;

  public:
    using script_type = build::script::script;

    // The prerequisite_target::include bits that indicate update=unmatch and
    // an ad hoc version of that.
    //
    static const uintptr_t include_unmatch       = 0x100;
    static const uintptr_t include_unmatch_adhoc = 0x200;


    script_type        script;
    string             checksum; // Script text hash.
    const target_type* ttype;    // First target/pattern type.
  };
}

#endif // LIBBUILD2_ADHOC_RULE_BUILDSCRIPT_HXX
