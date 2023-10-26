// file      : libbuild2/bash/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BASH_RULE_HXX
#define LIBBUILD2_BASH_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/install/rule.hxx>

#include <libbuild2/in/rule.hxx>

#include <libbuild2/bash/export.hxx>

namespace build2
{
  namespace bash
  {
    // Preprocess a bash script (exe{}) or module (bash{}) .in file that
    // imports one or more bash modules.
    //
    // Note that the default substitution symbol is '@' and the mode is lax
    // (think bash arrays). The idea is that '@' is normally used in ways that
    // are highly unlikely to be misinterpreted as substitutions. The user,
    // however, is still able to override both of these choices with the
    // corresponding in.* variables (e.g., to use '`' and strict mode).
    //
    class LIBBUILD2_BASH_SYMEXPORT in_rule: public in::rule
    {
    public:
      in_rule (): rule ("bash.in 1", "bash", '@', false /* strict */) {}

      virtual bool
      match (action, target&, const string&, match_extra&) const override;

      using in::rule::match; // Make Clang happy.

      virtual recipe
      apply (action, target&) const override;

      virtual prerequisite_target
      search (action,
              const target&,
              const prerequisite_member&,
              include_type) const override;

      virtual optional<string>
      substitute (const location&,
                  action a,
                  const target&,
                  const string&,
                  optional<uint64_t>,
                  bool,
                  const substitution_map*,
                  const optional<string>&) const override;

      string
      substitute_import (const location&,
                         action a,
                         const target&,
                         const string&) const;
    };

    // Installation rule for bash scripts (exe{}) and modules (bash{}) that
    // signals to in_rule that this is update for install.
    //
    class LIBBUILD2_BASH_SYMEXPORT install_rule: public install::file_rule
    {
    public:
      install_rule (const in_rule& r, const char* n): in_ (r), in_name_ (n) {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&, match_extra&) const override;

    protected:
      const in_rule& in_;
      const string in_name_;
    };
  }
}

#endif // LIBBUILD2_BASH_RULE_HXX
