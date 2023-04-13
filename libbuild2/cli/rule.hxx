// file      : libbuild2/cli/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CLI_RULE_HXX
#define LIBBUILD2_CLI_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/cli/export.hxx>

namespace build2
{
  namespace cli
  {
    // Cached data shared between rules and the module.
    //
    struct data
    {
      const exe&    ctgt; // CLI compiler target.
      const string& csum; // CLI compiler checksum.
    };

    // @@ Redo as two separate rules?
    //
    class LIBBUILD2_CLI_SYMEXPORT compile_rule: public simple_rule,
                                                private virtual data
    {
    public:
      compile_rule (data&& d): data (move (d)) {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;

      target_state
      perform_update (action, const target&) const;
    };
  }
}

#endif // LIBBUILD2_CLI_RULE_HXX
