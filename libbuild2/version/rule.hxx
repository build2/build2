// file      : libbuild2/version/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_VERSION_RULE_HXX
#define LIBBUILD2_VERSION_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/install/rule.hxx>

#include <libbuild2/in/rule.hxx>

namespace build2
{
  namespace version
  {
    // Preprocess an .in file that depends on manifest.
    //
    class in_rule: public in::rule
    {
    public:
      in_rule (): rule ("version.in 2", "version") {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;

      virtual string
      lookup (const location&,
              action,
              const target&,
              const string&,
              optional<uint64_t>,
              const substitution_map*,
              const optional<string>&) const override;
    };

    // Pre-process manifest before installation to patch in the version.
    //
    class manifest_install_rule: public install::file_rule
    {
    public:
      manifest_install_rule () {}

      virtual bool
      match (action, target&) const override;

      virtual auto_rmfile
      install_pre (const file&, const install_dir&) const override;
    };
  }
}

#endif // LIBBUILD2_VERSION_RULE_HXX
