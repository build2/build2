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
    // "Fail rule" for obj{}, [h]bmi{}, and libu{} that issues diagnostics if
    // someone tries to build any of these groups directly.
    //
    class fail_rule: public rule
    {
    public:
      fail_rule () {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;
    };

    // Pass-through to group members rule, similar to alias.
    //
    class LIBBUILD2_BIN_SYMEXPORT lib_rule: public rule
    {
    public:
      lib_rule () {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform (action, const target&);

      // Return library types to build according to the bin.lib value (set
      // on project's root scope by init()).
      //
      struct members
      {
        bool a; // static
        bool s; // shared
      };

      static members
      build_members (const scope&);
    };
  }
}

#endif // LIBBUILD2_BIN_RULE_HXX
