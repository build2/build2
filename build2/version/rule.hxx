// file      : build2/version/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_VERSION_RULE_HXX
#define BUILD2_VERSION_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>

namespace build2
{
  namespace version
  {
    // Generate a version file.
    //
    class doc_rule: public rule
    {
    public:
      doc_rule () {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_update (action, const target&);
    };

    // Preprocess an .in file.
    //
    class in_rule: public rule
    {
    public:
      in_rule () {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_update (action, const target&);
    };
  }
}

#endif // BUILD2_VERSION_RULE_HXX
