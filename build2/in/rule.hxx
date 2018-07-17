// file      : build2/in/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_IN_RULE_HXX
#define BUILD2_IN_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>

namespace build2
{
  namespace in
  {
    // Preprocess an .in file.
    //
    class rule: public build2::rule
    {
    public:
      rule (char symbol = '$', bool strict = true)
          : symbol_ (symbol), strict_ (strict) {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      // Perform variable lookup.
      //
      virtual string
      lookup (const location&, const target&, const string& name) const;

      // Perform variable substitution. Return nullopt if it should be
      // ignored.
      //
      virtual optional<string>
      substitute (const location&,
                  const target&,
                  const string& name,
                  bool strict) const;

      target_state
      perform_update (action, const target&) const;

    protected:
      char symbol_;
      bool strict_;
    };
  }
}

#endif // BUILD2_IN_RULE_HXX
