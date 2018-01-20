// file      : build2/install/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_INSTALL_RULE_HXX
#define BUILD2_INSTALL_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>
#include <build2/target.hxx>
#include <build2/operation.hxx>

namespace build2
{
  namespace install
  {
    class alias_rule: public rule
    {
    public:
      virtual bool
      match (action, target&, const string&) const override;

      // Return NULL if this prerequisite should be ignored and pointer to its
      // target otherwise. The default implementation accepts all prerequsites.
      //
      virtual const target*
      filter (action, const target&, prerequisite_member) const;

      virtual recipe
      apply (action, target&) const override;

      alias_rule () {}
      static const alias_rule instance;
    };

    // In addition to the alias rule's semantics, this rule sees through to
    // the group's members.
    //
    class group_rule: public alias_rule
    {
    public:
      // Return NULL if this group member should be ignored and pointer to its
      // target otherwise. The default implementation accepts all members.
      //
      virtual const target*
      filter (action, const target&, const target& group_member) const;

      virtual recipe
      apply (action, target&) const override;

      group_rule () {}
      static const group_rule instance;
    };

    struct install_dir;

    class file_rule: public rule
    {
    public:
      virtual bool
      match (action, target&, const string&) const override;

      // Return NULL if this prerequisite should be ignored and pointer to its
      // target otherwise. The default implementation ignores prerequsites
      // that are outside of this target's project.
      //
      virtual const target*
      filter (action, const target&, prerequisite_member) const;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_update (action, const target&);

      // Extra un/installation hooks. Return true if anything was
      // un/installed.
      //
      using install_dir = install::install_dir; // For derived rules.

      virtual bool
      install_extra (const file&, const install_dir&) const;

      virtual bool
      uninstall_extra (const file&, const install_dir&) const;

      // Installation/uninstallation "commands".
      //
      // If verbose is false, then only print the command at verbosity level 2
      // or higher.

      // Install a symlink: base/link -> target.
      //
      static void
      install_l (const scope& rs,
                 const install_dir& base,
                 const path& target,
                 const path& link,
                 bool verbose);

      // Uninstall a file or symlink:
      //
      // uninstall <target> <base>/  rm <base>/<target>.leaf (); name empty
      // uninstall <target> <name>   rm <base>/<name>; target can be NULL
      //
      // Return false if nothing has been removed (i.e., the file does not
      // exist).
      //
      static bool
      uninstall_f (const scope& rs,
                   const install_dir& base,
                   const file* target,
                   const path& name,
                   bool verbose);

      target_state
      perform_install (action, const target&) const;

      target_state
      perform_uninstall (action, const target&) const;

      static const file_rule instance;
      file_rule () {}
    };
  }
}

#endif // BUILD2_INSTALL_RULE_HXX
