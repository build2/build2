// file      : libbuild2/cc/install-rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_INSTALL_RULE_HXX
#define LIBBUILD2_CC_INSTALL_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/install/rule.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/common.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  namespace cc
  {
    class link_rule;

    // Installation rule for exe{} and lib[as]{}. Here we do:
    //
    // 1. Signal to the link rule that this is update for install.
    //
    // 2. Custom filtering of prerequisites (e.g., headers of an exe{}).
    //
    // 3. Extra un/installation (e.g., libs{} symlinks).
    //
    // 4. Handling runtime/buildtime match options for lib[as]{}.
    //
    class LIBBUILD2_CC_SYMEXPORT install_rule: public install::file_rule,
                                               virtual common
    {
    public:
      install_rule (data&&, const link_rule&);

      virtual bool
      filter (action, const target&, const target&) const override;

      virtual pair<const target*, uint64_t>
      filter (const scope*,
              action, const target&, prerequisite_iterator&,
              match_extra&) const override;

      // Note: rule::match() override (with hint and match_extra).
      //
      virtual bool
      match (action, target&, const string&, match_extra&) const override;

      using file_rule::match; // Make Clang happy.

      virtual recipe
      apply (action, target&, match_extra&) const override;

      virtual void
      apply_posthoc (action, target&, match_extra&) const override;

      virtual void
      reapply (action, target&, match_extra&) const override;

      virtual bool
      install_extra (const file&, const install_dir&) const override;

      virtual bool
      uninstall_extra (const file&, const install_dir&) const override;

    private:
      const link_rule& link_;
    };

    // Installation rule for libu[eas]{}.
    //
    // While libu*{} members themselves are not installable, we need to see
    // through them in case they depend on stuff that we need to install
    // (e.g., headers). Note that we use the alias_rule as a base.
    //
    class LIBBUILD2_CC_SYMEXPORT libux_install_rule: public install::alias_rule,
                                                     virtual common
    {
    public:
      libux_install_rule (data&&, const link_rule&);

      // Note: utility libraries currently have no ad hoc members.

      virtual pair<const target*, uint64_t>
      filter (const scope*,
              action, const target&, prerequisite_iterator&,
              match_extra&) const override;

      // Note: rule::match() override.
      //
      virtual bool
      match (action, target&, const string&, match_extra&) const override;

      using alias_rule::match; // Make Clang happy.

      virtual recipe
      apply (action, target&, match_extra&) const override;

      virtual void
      apply_posthoc (action, target&, match_extra&) const override;

      virtual void
      reapply (action, target&, match_extra&) const override;

    private:
      const link_rule& link_;
    };
  }
}

#endif // LIBBUILD2_CC_INSTALL_RULE_HXX
