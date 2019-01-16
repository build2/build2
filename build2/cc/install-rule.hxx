// file      : build2/cc/install-rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_INSTALL_RULE_HXX
#define BUILD2_CC_INSTALL_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/install/rule.hxx>

#include <build2/cc/types.hxx>
#include <build2/cc/common.hxx>

namespace build2
{
  namespace cc
  {
    class link_rule;

    // Installation rule for exe{} and lib*{}. Here we do:
    //
    // 1. Signal to the link rule that this is update for install.
    //
    // 2. Custom filtering of prerequisites (e.g., headers of an exe{}).
    //
    // 3. Extra un/installation (e.g., libs{} symlinks).
    //
    class install_rule: public install::file_rule, virtual common
    {
    public:
      install_rule (data&&, const link_rule&);

      virtual const target*
      filter (action, const target&, prerequisite_iterator&) const override;

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      virtual bool
      install_extra (const file&, const install_dir&) const override;

      virtual bool
      uninstall_extra (const file&, const install_dir&) const override;

    private:
      const link_rule& link_;
    };

    // Installation rule for libu*{}.
    //
    // While libu*{} members themselves are not installable, we need to see
    // through them in case they depend on stuff that we need to install
    // (e.g., headers). Note that we use the alias_rule as a base.
    //
    class libux_install_rule: public install::alias_rule, virtual common
    {
    public:
      libux_install_rule (data&&, const link_rule&);

      virtual const target*
      filter (action, const target&, prerequisite_iterator&) const override;

      virtual bool
      match (action, target&, const string&) const override;

    private:
      const link_rule& link_;
    };
  }
}

#endif // BUILD2_CC_INSTALL_RULE_HXX
