// file      : libbuild2/dist/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DIST_RULE_HXX
#define LIBBUILD2_DIST_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/action.hxx>
#include <libbuild2/target.hxx>

namespace build2
{
  namespace dist
  {
    // This is the default rule that simply matches all the prerequisites.
    //
    // A custom rule (usually the same as perform_update) may be necessary to
    // establish group links (so that we see the dist variable set on a group)
    // or to see through non-see-through groups (like lib{}; see the
    // bin::lib_rule for an example). Note that in the latter case the rule
    // should "see" all its members for the dist case.
    //
    class rule: public build2::rule
    {
    public:
      rule () {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;
    };
  }
}

#endif // LIBBUILD2_DIST_RULE_HXX
