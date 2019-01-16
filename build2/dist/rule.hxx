// file      : build2/dist/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_DIST_RULE_HXX
#define BUILD2_DIST_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>
#include <build2/action.hxx>
#include <build2/target.hxx>

namespace build2
{
  namespace dist
  {
    // This is the default rule that simply matches all the prerequisites.
    //
    // A custom rule (usually the same as perform_update) may be necessary to
    // establish group links (so that we see the dist variable set on a
    // group).
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

#endif // BUILD2_DIST_RULE_HXX
