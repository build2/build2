// file      : libbuild2/dist/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DIST_RULE_HXX
#define LIBBUILD2_DIST_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/action.hxx>
#include <libbuild2/target.hxx>

#include <libbuild2/dist/types.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  namespace dist
  {
    // This is the default rule that simply matches all the prerequisites.
    //
    // A custom rule (usually the same as perform_update) may be necessary to
    // establish group links (so that we see the dist variable set on a group)
    // or to see through non-see-through groups (like lib{}, obj{}; see rule
    // in the bin module for an example). Note that in the latter case the
    // rule should "see" all its members for the dist case.
    //
    class LIBBUILD2_SYMEXPORT rule: public simple_rule
    {
    public:
      rule () {}

      // Always matches (returns true).
      //
      virtual bool
      match (action, target&) const override;

      // Matches all the prerequisites (including from group) and returns
      // noop_recipe (which will never be executed).
      //
      virtual recipe
      apply (action, target&) const override;

      static void
      match_postponed (const postponed_prerequisite&);
    };
  }
}

#endif // LIBBUILD2_DIST_RULE_HXX
