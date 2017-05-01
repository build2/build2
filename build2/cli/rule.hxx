// file      : build2/cli/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CLI_RULE_HXX
#define BUILD2_CLI_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>

namespace build2
{
  namespace cli
  {
    // @@ Redo as two separate rules?
    //
    class compile: public rule
    {
    public:
      compile () {}

      virtual match_result
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_update (action, const target&);

      static target_state
      perform_clean (action, const target&);
    };
  }
}

#endif // BUILD2_CLI_RULE_HXX
