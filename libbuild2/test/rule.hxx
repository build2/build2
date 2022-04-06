// file      : libbuild2/test/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_RULE_HXX
#define LIBBUILD2_TEST_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/action.hxx>

#include <libbuild2/test/common.hxx>

namespace build2
{
  namespace test
  {
    class rule: public simple_rule, protected virtual common
    {
    public:
      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_update (action, const target&, size_t);

      target_state
      perform_test (action, const target&, size_t) const;

      target_state
      perform_script (action, const target&, size_t) const;

      rule (common_data&& d, bool sto)
          : common (move (d)), see_through_only (sto) {}

      bool see_through_only;
    };

    class default_rule: public rule
    {
    public:
      explicit
      default_rule (common_data&& d)
          : common (move (d)),
            rule (move (d), true /* see_through_only */) {}
    };

    // To be used for non-see-through groups that should exhibit the see-
    // through behavior for install (see lib{} in the bin module for an
    // example).
    //
    class group_rule: public rule
    {
    public:
      explicit
      group_rule (common_data&& d)
          : common (move (d)), rule (move (d), false /* see_through_only */) {}
    };
  }
}

#endif // LIBBUILD2_TEST_RULE_HXX
