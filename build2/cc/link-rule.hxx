// file      : build2/cc/link-rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_LINK_RULE_HXX
#define BUILD2_CC_LINK_RULE_HXX

#include <set>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>

#include <build2/cc/types.hxx>
#include <build2/cc/common.hxx>

namespace build2
{
  namespace cc
  {
    class link_rule: public rule, virtual common
    {
    public:
      link_rule (data&&);

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      target_state
      perform_update (action, const target&) const;

      target_state
      perform_clean (action, const target&) const;

    private:
      friend class install_rule;
      friend class libux_install_rule;

      // Shared library paths.
      //
      struct libs_paths
      {
        // If any (except real) is empty, then it is the same as the next
        // one. Except for intermediate, for which empty indicates that it is
        // not used.
        //
        // The libs{} path is always the real path. On Windows the link path
        // is the import library.
        //
        path        link;   // What we link: libfoo.so
        path        soname; // SONAME:       libfoo-1.so, libfoo.so.1
        path        interm; // Intermediate: libfoo.so.1.2
        const path* real;   // Real:         libfoo.so.1.2.3

        inline const path&
        effect_link () const {return link.empty () ? effect_soname () : link;}

        inline const path&
        effect_soname () const {return soname.empty () ? *real : soname;}

        // Cleanup pattern used to remove previous versions. If empty, no
        // cleanup is performed. The above (current) names are automatically
        // filtered out.
        //
        path clean;
      };

      libs_paths
      derive_libs_paths (file&, const char*, const char*) const;

      struct match_data
      {
        // The "for install" condition is signalled to us by install_rule when
        // it is matched for the update operation. It also verifies that if we
        // have already been executed, then it was for install.
        //
        // This has an interesting implication: it means that this rule cannot
        // be used to update targets during match. Specifically, we cannot be
        // executed for group resolution purposes (not a problem) nor as part
        // of the generated source update. The latter case can be a problem:
        // imagine a code generator that itself may need to be updated before
        // it can be used to re-generate some out-of-date source code. As an
        // aside, note that even if we were somehow able to communicate the
        // "for install" in this case, the result of such an update may not
        // actually be "usable" (e.g., not runnable because of the missing
        // rpaths). There is another prominent case where the result may not
        // be usable: cross-compilation.
        //
        // So the current (admittedly fuzzy) thinking is that a project shall
        // not try to use its own build for update since it may not be usable
        // (because of cross-compilations, being "for install", etc). Instead,
        // it should rely on another, "usable" build of itself (this, BTW, is
        // related to bpkg's build-time vs run-time dependencies).
        //
        optional<bool> for_install;

        bool binless; // Binary-less library.

        libs_paths libs_data;
      };

      // Library handling.
      //
      void
      append_libraries (strings&,
                        const file&, bool, lflags,
                        const scope&, action, linfo) const;

      void
      hash_libraries (sha256&,
                      bool&, timestamp,
                      const file&, bool, lflags,
                      const scope&, action, linfo) const;

      void
      rpath_libraries (strings&,
                       const target&,
                       const scope&, action, linfo,
                       bool) const;

      // Windows rpath emulation (windows-rpath.cxx).
      //
      struct windows_dll
      {
        const string& dll;
        const string* pdb; // NULL if none.
        string pdb_storage;

        bool operator< (const windows_dll& y) const {return dll < y.dll;}
      };

      using windows_dlls = std::set<windows_dll>;

      timestamp
      windows_rpath_timestamp (const file&,
                               const scope&,
                               action, linfo) const;

      windows_dlls
      windows_rpath_dlls (const file&, const scope&, action, linfo) const;

      void
      windows_rpath_assembly (const file&, const scope&, action, linfo,
                              const string&,
                              timestamp,
                              bool) const;

      // Windows-specific (windows-manifest.cxx).
      //
      pair<path, bool>
      windows_manifest (const file&, bool rpath_assembly) const;

      // pkg-config's .pc file generation (pkgconfig.cxx).
      //
      void
      pkgconfig_save (action, const file&, bool, bool) const;

    private:
      const string rule_id;
    };
  }
}

#endif // BUILD2_CC_LINK_RULE_HXX
