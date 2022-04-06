// file      : libbuild2/bin/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_RULE_HXX
#define LIBBUILD2_BIN_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/bin/export.hxx>

namespace build2
{
  namespace bin
  {
    // "Fail rule" for obj{} and [h]bmi{} that issues diagnostics if someone
    // tries to build these groups directly.
    //
    class obj_rule: public simple_rule
    {
    public:
      obj_rule () {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;
    };

    // "Fail rule" for libul{} that issues diagnostics if someone tries to
    // build this group directly.
    //
    class libul_rule: public simple_rule
    {
    public:
      libul_rule () {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;
    };

    // Pass-through to group members rule, similar to alias.
    //
    class LIBBUILD2_BIN_SYMEXPORT lib_rule: public simple_rule
    {
    public:
      lib_rule () {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform (action, const target&);
    };
  }
}

#endif // LIBBUILD2_BIN_RULE_HXX
