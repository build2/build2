// file      : build2/test/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TEST_RULE_HXX
#define BUILD2_TEST_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>
#include <build2/operation.hxx>

#include <build2/test/common.hxx>

namespace build2
{
  namespace test
  {
    class rule: public build2::rule, protected virtual common
    {
    public:
      explicit
      rule (common_data&& d): common (move (d)) {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_update (action, const target&);

      target_state
      perform_test (action, const target&, size_t) const;

      target_state
      perform_script (action, const target&, size_t) const;
    };

    class default_rule: public rule // For disambiguation in module.
    {
    public:
      explicit
      default_rule (common_data&& d): common (move (d)), rule (move (d)) {}
    };

    // In addition to the above rule's semantics, this rule sees through to
    // the group's members.
    //
    class group_rule: public rule
    {
    public:
      explicit
      group_rule (common_data&& d): common (move (d)), rule (move (d)) {}

      virtual recipe
      apply (action, target&) const override;
    };
  }
}

#endif // BUILD2_TEST_RULE_HXX
