// file      : build2/cc/link.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_LINK_HXX
#define BUILD2_CC_LINK_HXX

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
    class link: public rule, virtual common
    {
    public:
      link (data&&);

      virtual match_result
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      target_state
      perform_update (action, const target&) const;

      target_state
      perform_clean (action, const target&) const;

    private:
      friend class file_install;
      friend class alias_install;

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
        const path  link;   // What we link: libfoo.so
        const path  soname; // SONAME:       libfoo-1.so, libfoo.so.1
        const path  interm; // Intermediate: libfoo.so.1.2
        const path& real;   // Real:         libfoo.so.1.2.3

        inline const path&
        effect_link () const {return link.empty () ? effect_soname () : link;}

        inline const path&
        effect_soname () const {return soname.empty () ? real : soname;}

        // Cleanup pattern used to remove previous versions. If empty, no
        // cleanup is performed. The above (current) names are automatically
        // filtered out.
        //
        const path clean;
      };

      libs_paths
      derive_libs_paths (file&, const char*, const char*) const;

      // Library handling.
      //
      void
      append_libraries (strings&,
                        const file&, bool, lflags,
                        const scope&, action, linfo) const;

      void
      hash_libraries (sha256&,
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

    private:
      const string rule_id;
    };
  }
}

#endif // BUILD2_CC_LINK_HXX
