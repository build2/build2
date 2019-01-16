// file      : build2/version/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_VERSION_RULE_HXX
#define BUILD2_VERSION_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/in/rule.hxx>
#include <build2/install/rule.hxx>

namespace build2
{
  namespace version
  {
    // Preprocess an .in file that depends on manifest.
    //
    class in_rule: public in::rule
    {
    public:
      in_rule (): rule ("version.in 1", "version.in") {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual string
      lookup (const location&,
              action,
              const target&,
              const string&) const override;
    };

    // Pre-process manifest before installation to patch in the version.
    //
    class manifest_install_rule: public install::file_rule
    {
    public:
      manifest_install_rule () {}

      virtual bool
      match (action, target&, const string&) const override;

      virtual auto_rmfile
      install_pre (const file&, const install_dir&) const override;
    };
  }
}

#endif // BUILD2_VERSION_RULE_HXX
