// file      : libbuild2/cc/link-rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_LINK_RULE_HXX
#define LIBBUILD2_CC_LINK_RULE_HXX

#include <set>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/common.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  namespace cc
  {
    class LIBBUILD2_CC_SYMEXPORT link_rule: public simple_rule, virtual common
    {
    public:
      link_rule (data&&);

      struct match_result
      {
        bool seen_x   = false;
        bool seen_c   = false;
        bool seen_cc  = false;
        bool seen_obj = false;
        bool seen_lib = false;

        match_result& operator|= (match_result y)
        {
          seen_x   = seen_x   || y.seen_x;
          seen_c   = seen_c   || y.seen_c;
          seen_cc  = seen_cc  || y.seen_cc;
          seen_obj = seen_obj || y.seen_obj;
          seen_lib = seen_lib || y.seen_lib;
          return *this;
        }
      };

      match_result
      match (action, const target&, const target*, otype, bool) const;

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      target_state
      perform_update (action, const target&) const;

      target_state
      perform_clean (action, const target&) const;

      using simple_rule::match; // To make Clang happy.

    public:
      // Library handling.
      //
      struct appended_library
      {
        static const size_t npos = size_t (~0);

        // Each appended_library represents either a library target or a
        // library name fragment up to 2 elements long:
        //
        //                          target         |         name
        //                --------------------------------------------------
        const void* l1; //      library target     | library name[1] or NULL
        const void* l2; //           NULL          | library name[0]

        size_t begin;  // First arg belonging to this library.
        size_t end;    // Past last arg belonging to this library.
      };

      class appended_libraries: public small_vector<appended_library, 128>
      {
      public:
        // Find existing or append new entry. If appending new, use the second
        // argument as the begin value.
        //
        appended_library&
        append (const file& l, size_t b)
        {
          auto i (find_if (begin (), end (),
                           [&l] (const appended_library& al)
                           {
                             return al.l2 == nullptr && al.l1 == &l;
                           }));

          if (i != end ())
            return *i;

          push_back (appended_library {&l, nullptr, b, appended_library::npos});
          return back ();
        }

        // Return NULL if no duplicate tracking can be performed for this
        // library.
        //
        appended_library*
        append (const small_vector<reference_wrapper<const string>, 2>& ns,
                size_t b)
        {
          size_t n (ns.size ());

          if (n > 2)
            return nullptr;

          auto i (
            find_if (
              begin (), end (),
              [&ns, n] (const appended_library& al)
              {
                return al.l2 != nullptr &&
                  *static_cast<const string*> (al.l2) == ns[0].get () &&
                  (n == 2
                   ? (al.l1 != nullptr &&
                      *static_cast<const string*> (al.l1) == ns[1].get ())
                   : al.l1 == nullptr);
              }));

          if (i != end ())
            return &*i;

          push_back (appended_library {
              n == 2 ? &ns[1].get () : nullptr, &ns[0].get (),
              b, appended_library::npos});

          return &back ();
        }
      };

      void
      append_libraries (appended_libraries&, strings&,
                        const scope&, action,
                        const file&, bool, lflags, linfo,
                        bool = true, bool = true) const;

      void
      append_libraries (sha256&, bool&, timestamp,
                        const scope&, action,
                        const file&, bool, lflags,  linfo) const;

      using rpathed_libraries = small_vector<const file*, 256>;

      void
      rpath_libraries (rpathed_libraries&, strings&,
                       const scope&,
                       action, const file&, bool, linfo, bool, bool) const;

      void
      rpath_libraries (strings&,
                       const scope&, action,
                       const target&, linfo, bool) const;

      void
      append_binless_modules (strings&,
                              const scope&, action, const file&) const;

      void
      append_binless_modules (sha256&,
                              const scope&, action, const file&) const;

    protected:
      static void
      functions (function_family&, const char*); // functions.cxx

    private:
      friend class install_rule;
      friend class libux_install_rule;

      // Shared library paths.
      //
      struct libs_paths
      {
        // If any (except real) is empty, then it is the same as the next
        // one. Except for load and intermediate, for which empty indicates
        // that it is not used.
        //
        // Note that the paths must form a "hierarchy" with subsequent paths
        // adding extra information as suffixes. This is relied upon by the
        // clean patterns (see below).
        //
        // The libs{} path is always the real path. On Windows what we link
        // to is the import library and the link path is empty.
        //
        path        link;   // What we link: libfoo.so
        path        load;   // What we load (with dlopen() or similar)
        path        soname; // SONAME:       libfoo-1.so, libfoo.so.1
        path        interm; // Intermediate: libfoo.so.1.2
        const path* real;   // Real:         libfoo.so.1.2.3

        inline const path&
        effect_link () const {return link.empty () ? effect_soname () : link;}

        inline const path&
        effect_soname () const {return soname.empty () ? *real : soname;}

        // Cleanup patterns used to remove previous load suffixes/versions.
        // If empty, no corresponding cleanup is performed. The current names
        // as well as names with the real path as a prefix are automatically
        // filtered out.
        //
        path clean_load;
        path clean_version;
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
        size_t start; // Parallel prerequisites/prerequisite_targets start.

        link_rule::libs_paths libs_paths;
      };

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
      pair<path, timestamp>
      windows_manifest (const file&, bool rpath_assembly) const;

      // pkg-config's .pc file generation (pkgconfig.cxx).
      //
      void
      pkgconfig_save (action, const file&, bool, bool, bool) const;

    private:
      const string rule_id;
    };
  }
}

#endif // LIBBUILD2_CC_LINK_RULE_HXX
