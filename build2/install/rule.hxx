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
      // The prerequisite it passed as an iterator allowing the filter to
      // "see" inside groups.
      //
      using prerequisite_iterator =
        prerequisite_members_range<group_prerequisites>::iterator;

      virtual const target*
      filter (action, const target&, prerequisite_iterator&) const;

      virtual const target*
      filter (action, const target&, const prerequisite&) const;

      virtual recipe
      apply (action, target&) const override;

      alias_rule () {}
      static const alias_rule instance;
    };

    // In addition to the alias rule's semantics, this rule sees through to
    // the group's members.
    //
    // The default group_rule::instance matches any target for which it was
    // registered. It is to be used for non-see-through groups that should
    // exhibit the see-through behavior for install (see lib{} in the bin
    // module for an example).
    //
    // We also register (for all targets) another instance of this rule that
    // only matches see-through groups.
    //
    class group_rule: public alias_rule
    {
    public:
      virtual bool
      match (action, target&, const string&) const override;

      // Return NULL if this group member should be ignored and pointer to its
      // target otherwise. The default implementation accepts all members.
      //
      virtual const target*
      filter (action, const target&, const target& group_member) const;

      using alias_rule::filter; // "Unhide" to make Clang happy.

      virtual recipe
      apply (action, target&) const override;

      group_rule (bool see_through_only): see_through (see_through_only) {}
      static const group_rule instance;

      bool see_through;
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
      // The prerequisite it passed as an iterator allowing the filter to
      // "see" inside groups.
      //
      using prerequisite_iterator =
        prerequisite_members_range<group_prerequisites>::iterator;

      virtual const target*
      filter (action, const target&, prerequisite_iterator&) const;

      virtual const target*
      filter (action, const target&, const prerequisite&) const;

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
