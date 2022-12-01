// file      : libbuild2/bin/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_RULE_HXX
#define LIBBUILD2_BIN_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/dist/rule.hxx>

#include <libbuild2/bin/export.hxx>

namespace build2
{
  namespace bin
  {
    // "Fail rule" for obj{} and [h]bmi{} that issues diagnostics if someone
    // tries to build these groups directly.
    //
    // Note that for dist it acts as a pass-through to all existing (declared)
    // members.
    //
    class obj_rule: public dist::rule
    {
    public:
      obj_rule () {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;
    };

    // This rule picks, matches, and unmatches (if possible) a member for the
    // purpose of making its metadata (for example, library's poptions, if
    // it's one of the cc libraries) available.
    //
    // The underlying idea here is that someone else (e.g., cc::link_rule)
    // makes a more informed choice and we piggy back on that decision,
    // falling back to making our own based on bin.lib and bin.exe.lib. Note
    // that for update this rule always returns target_state::unchanged.
    //
    // Note also that for dist it acts as a pass-through to all existing
    // (declared) members.
    //
    class libul_rule: public dist::rule
    {
    public:
      explicit
      libul_rule () {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;
    };

    // Pass-through to group members rule, similar to alias.
    //
    // Note that for dist it always passes to both members.
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
