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
    class rule_common: public build2::rule, protected virtual common
    {
    public:
      virtual match_result
      match (action, target&, const string&) const override;

      target_state
      perform_script (action, const target&) const;
    };

    class rule: public rule_common
    {
    public:
      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_test (action, const target&);
    };

    class alias_rule: public rule_common
    {
    public:
      virtual recipe
      apply (action, target&) const override;

      target_state
      perform_test (action, const target&) const;
    };
  }
}

#endif // BUILD2_TEST_RULE_HXX
