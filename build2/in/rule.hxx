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
    // Note that a derived rule can use the target data pad to cache data
    // (e.g., in match()) to be used in substitute/lookup() calls.
    //
    class rule: public build2::rule
    {
    public:
      // The rule id is used to form the rule name/version entry in depdb. The
      // program argument is the pseudo-program name to use in the command
      // line diagnostics.
      //
      rule (string rule_id,
            string program,
            char symbol = '$',
            bool strict = true)
          : rule_id_ (move (rule_id)),
            program_ (move (program)),
            symbol_ (symbol),
            strict_ (strict) {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      virtual target_state
      perform_update (action, const target&) const;

      // Customization hooks.
      //

      // Perform prerequisite search.
      //
      virtual prerequisite_target
      search (action,
              const target&,
              const prerequisite_member&,
              include_type) const;

      // Perform variable lookup.
      //
      virtual string
      lookup (const location&,
              action,
              const target&,
              const string& name) const;

      // Perform variable substitution. Return nullopt if it should be
      // ignored.
      //
      virtual optional<string>
      substitute (const location&,
                  action,
                  const target&,
                  const string& name,
                  bool strict) const;

    protected:
      const string rule_id_;
      const string program_;
      char symbol_;
      bool strict_;
    };
  }
}

#endif // BUILD2_IN_RULE_HXX
