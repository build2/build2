// file      : libbuild2/cc/link-rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_LINK_RULE_HXX
#define LIBBUILD2_CC_LINK_RULE_HXX

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
    class LIBBUILD2_CC_SYMEXPORT link_rule: public rule, virtual common
    {
    public:
      link_rule (data&&);

      struct match_data;

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
      match (action, target&, const string&, match_extra&) const override;

      virtual recipe
      apply (action, target&, match_extra&) const override;

      target_state
      perform_update (action, const target&, match_data&) const;

      target_state
      perform_clean (action, const target&, match_data&) const;

      virtual const target*
      import (const prerequisite_key&,
              const optional<string>&,
              const location&) const override;

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
        // Find existing entry, if any.
        //
        appended_library*
        find (const file& l)
        {
          auto i (find_if (begin (), end (),
                           [&l] (const appended_library& al)
                           {
                             return al.l2 == nullptr && al.l1 == &l;
                           }));

          return i != end () ? &*i : nullptr;
        }

        appended_library*
        find (const small_vector<reference_wrapper<const string>, 2>& ns)
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

          return i != end () ? &*i : nullptr;
        }

        // Find existing or append new entry. If appending new, use the second
        // argument as the begin value.
        //
        appended_library&
        append (const file& l, size_t b)
        {
          if (appended_library* r = find (l))
            return *r;

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

          if (appended_library* r = find (ns))
            return r;

          push_back (appended_library {
              n == 2 ? &ns[1].get () : nullptr, &ns[0].get (),
              b, appended_library::npos});

          return &back ();
        }

        // Hoist the elements corresponding to the specified library to the
        // end.
        //
        void
        hoist (strings& args, appended_library& al)
        {
          if (al.begin != al.end)
          {
            // Rotate to the left the subrange starting from the first element
            // of this library and until the end so that the element after the
            // last element of this library becomes the first element of this
            // subrange. We also need to adjust begin/end of libraries
            // affected by the rotation.
            //
            rotate (args.begin () + al.begin,
                    args.begin () + al.end,
                    args.end ());

            size_t n (al.end - al.begin);

            for (appended_library& al1: *this)
            {
              if (al1.begin >= al.end)
              {
                al1.begin -= n;
                al1.end -= n;
              }
            }

            al.end = args.size ();
            al.begin = al.end - n;
          }
        }
      };

      void
      append_libraries (appended_libraries&, strings&,
                        sha256*, bool*, timestamp,
                        const scope&, action,
                        const file&, bool, lflags, linfo,
                        optional<bool>, bool = true, bool = true,
                        library_cache* = nullptr) const;

      using rpathed_libraries = small_vector<const file*, 256>;

      void
      rpath_libraries (rpathed_libraries&, strings&,
                       const scope&,
                       action, const file&, bool, linfo, bool, bool,
                       library_cache* = nullptr) const;

      void
      rpath_libraries (strings&,
                       const scope&, action,
                       const target&, linfo, bool) const;

      void
      append_binless_modules (strings&, sha256*,
                              const scope&, action, const file&) const;

      bool
      deduplicate_export_libs (
        const scope&,
        const vector<name>&,
        names&,
        vector<reference_wrapper<const name>>* = nullptr) const;

      optional<path>
      find_system_library (const strings&) const;

    protected:
      static void
      functions (function_family&, const char*); // functions.cxx

      // Implementation details.
      //
    public:

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
        explicit
        match_data (const link_rule& r): rule (r) {}

        // The "for install" condition is signalled to us by install_rule when
        // it is matched for the update operation. It also verifies that if we
        // have already been executed, then it was for install.
        //
        // This has an interesting implication: it means that this rule cannot
        // be used to update targets to be installed during match (since we
        // would notice that they are for install too late). Specifically, we
        // cannot be executed for group resolution purposes (should not be a
        // problem) nor as part of the generated source update. The latter
        // case can be a problem: imagine a source code generator that itself
        // may need to be updated before it can be used to re-generate some
        // out-of-date source code (or, worse, both the generator and the
        // target to be installed depend on the same library).
        //
        // As an aside, note that even if we were somehow able to communicate
        // the "for install" in this case, the result of such an update may
        // not actually be "usable" (e.g., not runnable because of the missing
        // rpaths). There is another prominent case where the result may not
        // be usable: cross-compilation (in fact, if you think about it, "for
        // install" is quite similar to cross-compilation: we are building for
        // a foreign "environment" and thus cannot execute the results of the
        // build).
        //
        // So the current thinking is that a project shall not try to use its
        // own "for install" (or, naturally, cross-compilation) build for
        // update since it may not be usable. Instead, it should rely on
        // another, "usable" build.
        //
        optional<bool> for_install;

        bool binless; // Binary-less library.
        size_t start; // Parallel prerequisites/prerequisite_targets start.

        link_rule::libs_paths libs_paths;

        const link_rule& rule;

        target_state
        operator() (action a, const target& t)
        {
          return a == perform_update_id
            ? rule.perform_update (a, t, *this)
            : rule.perform_clean (a, t, *this);
        }
      };

      // Windows rpath emulation (windows-rpath.cxx).
      //
     private:
      struct windows_dll
      {
        reference_wrapper<const string> dll;
        string                          pdb; // Empty if none.
      };

      using windows_dlls = vector<windows_dll>;

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
