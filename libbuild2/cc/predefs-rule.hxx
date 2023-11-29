// file      : libbuild2/cc/predefs-rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_PREDEFS_RULE_HXX
#define LIBBUILD2_CC_PREDEFS_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/common.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  namespace cc
  {
    class LIBBUILD2_CC_SYMEXPORT predefs_rule: public rule,
                                               virtual common
    {
    public:
      const string rule_name;

      explicit
      predefs_rule (data&&);

      virtual bool
      match (action, target&, const string&, match_extra&) const override;

      virtual recipe
      apply (action, target&, match_extra&) const override;

      target_state
      perform_update (action, const target&) const;

    private:
      const string rule_id;
    };
  }
}

#endif // LIBBUILD2_CC_PREDEFS_RULE_HXX
