// file      : libbuild2/bin/def-rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_DEF_RULE_HXX
#define LIBBUILD2_BIN_DEF_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/bin/export.hxx>

namespace build2
{
  namespace bin
  {
    // Generate a .def file from one or more object files and/or utility
    // libraries that exports all their symbols.
    //
    class LIBBUILD2_BIN_SYMEXPORT def_rule: public simple_rule
    {
    public:
      def_rule () {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_update (action, const target&);

    private:
      static const string rule_id_;
    };
  }
}

#endif // LIBBUILD2_BIN_DEF_RULE_HXX
