// file      : build2/cc/common.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_COMMON_HXX
#define BUILD2_CC_COMMON_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/variable.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/types.hxx>
#include <build2/cc/guess.hxx> // compiler_id

namespace build2
{
  namespace cc
  {
    // Data entries that define a concrete c-family module (e.g., c or cxx).
    // These classes are used as a virtual bases by the rules as well as the
    // modules. This way the member variables can be referenced as is, without
    // any extra decorations (in other words, it is a bunch of data members
    // that can be shared between several classes/instances).
    //
    struct config_data
    {
      lang x_lang;

      const char* x;         // Module name ("c", "cxx").
      const char* x_name;    // Compiler name ("c", "c++").
      const char* x_default; // Compiler default ("gcc", "g++").
      const char* x_pext;    // Preprocessed source extension (".i", ".ii").

      // Array of modules that can hint us the toolchain, terminate with
      // NULL.
      //
      const char* const* x_hinters;

      const variable& config_x;
      const variable& config_x_id;      // <type>[-<variant>]
      const variable& config_x_version;
      const variable& config_x_target;
      const variable& config_x_poptions;
      const variable& config_x_coptions;
      const variable& config_x_loptions;
      const variable& config_x_libs;

      const variable& x_path;         // Compiler process path.
      const variable& x_sys_lib_dirs; // System library search directories.
      const variable& x_sys_inc_dirs; // System header search directories.

      const variable& x_poptions;
      const variable& x_coptions;
      const variable& x_loptions;
      const variable& x_libs;

      const variable& c_poptions; // cc.*
      const variable& c_coptions;
      const variable& c_loptions;
      const variable& c_libs;

      const variable& x_export_poptions;
      const variable& x_export_coptions;
      const variable& x_export_loptions;
      const variable& x_export_libs;

      const variable& c_export_poptions; // cc.export.*
      const variable& c_export_coptions;
      const variable& c_export_loptions;
      const variable& c_export_libs;

      const variable& x_stdlib;       // x.stdlib

      const variable& c_runtime;      // cc.runtime
      const variable& c_stdlib;       // cc.stdlib

      const variable& c_type;         // cc.type
      const variable& c_system;       // cc.system
      const variable& c_module_name;  // cc.module_name
      const variable& c_reprocess;    // cc.reprocess

      const variable& x_preprocessed; // x.preprocessed
      const variable* x_symexport;    // x.features.symexport

      const variable& x_std;

      const variable& x_id;
      const variable& x_id_type;
      const variable& x_id_variant;

      const variable& x_class;

      const variable& x_version;
      const variable& x_version_major;
      const variable& x_version_minor;
      const variable& x_version_patch;
      const variable& x_version_build;

      const variable& x_signature;
      const variable& x_checksum;

      const variable& x_pattern;

      const variable& x_target;
      const variable& x_target_cpu;
      const variable& x_target_vendor;
      const variable& x_target_system;
      const variable& x_target_version;
      const variable& x_target_class;
    };

    struct data: config_data
    {
      const char* x_compile; // Rule names.
      const char* x_link;
      const char* x_install;
      const char* x_uninstall;

      // Cached values for some commonly-used variables/values.
      //

      compiler_type ctype;          // x.id.type
      const string& cvariant;       // x.id.variant
      compiler_class cclass;        // x.class
      uint64_t cmaj;                // x.version.major
      uint64_t cmin;                // x.version.minor
      const process_path& cpath;    // x.path

      const target_triplet& ctgt;   // x.target
      const string& tsys;           // x.target.system
      const string& tclass;         // x.target.class

      const strings& tstd;          // Translated x_std value (options).

      bool modules;                 // x.features.modules
      bool symexport;               // x.features.symexport

      const dir_paths& sys_lib_dirs; // x.sys_lib_dirs
      const dir_paths& sys_inc_dirs; // x.sys_inc_dirs

      size_t sys_lib_dirs_extra;     // First extra path (size if none).
      size_t sys_inc_dirs_extra;     // First extra path (size if none).

      const target_type& x_src; // Source target type (c{}, cxx{}).
      const target_type* x_mod; // Module target type (mxx{}), if any.

      // Array of target types that are considered headers. Keep them in the
      // most likely to appear order with the "header header" first and
      // terminated with NULL.
      //
      const target_type* const* x_hdr;

      template <typename T>
      bool
      x_header (const T& t) const
      {
        for (const target_type* const* ht (x_hdr); *ht != nullptr; ++ht)
          if (t.is_a (**ht))
            return true;

        return false;
      }

      // Array of target types that can be #include'd. Used to reverse-lookup
      // extensions to target types. Keep them in the most likely to appear
      // order and terminate with NULL.
      //
      const target_type* const* x_inc;

      // Aggregate-like constructor with from-base support.
      //
      data (const config_data& cd,
            const char* compile,
            const char* link,
            const char* install,
            const char* uninstall,
            compiler_type ct,
            const string& cv,
            compiler_class cl,
            uint64_t mj, uint64_t mi,
            const process_path& path,
            const target_triplet& tgt,
            const strings& std,
            bool fm,
            bool fs,
            const dir_paths& sld,
            const dir_paths& sid,
            size_t sle,
            size_t sie,
            const target_type& src,
            const target_type* mod,
            const target_type* const* hdr,
            const target_type* const* inc)
          : config_data (cd),
            x_compile (compile),
            x_link (link),
            x_install (install),
            x_uninstall (uninstall),
            ctype (ct), cvariant (cv), cclass (cl),
            cmaj (mj), cmin (mi),
            cpath (path),
            ctgt (tgt), tsys (ctgt.system), tclass (ctgt.class_),
            tstd (std),
            modules (fm),
            symexport (fs),
            sys_lib_dirs (sld), sys_inc_dirs (sid),
            sys_lib_dirs_extra (sle), sys_inc_dirs_extra (sie),
            x_src (src), x_mod (mod), x_hdr (hdr), x_inc (inc) {}
    };

    class common: public data
    {
    public:
      common (data&& d): data (move (d)) {}

      // Library handling.
      //
    public:
      void
      process_libraries (
        action,
        const scope&,
        linfo,
        const dir_paths&,
        const file&,
        bool,
        lflags,
        const function<bool (const file&, bool)>&,
        const function<void (const file* const*, const string&, lflags, bool)>&,
        const function<void (const file&, const string&, bool, bool)>&,
        bool = false,
        small_vector<const file*, 16>* = nullptr) const;

      const target*
      search_library (action a,
                      const dir_paths& sysd,
                      optional<dir_paths>& usrd,
                      const prerequisite& p) const
      {
        const target* r (p.target.load (memory_order_consume));

        if (r == nullptr)
        {
          if ((r = search_library (a, sysd, usrd, p.key ())) != nullptr)
          {
            const target* e (nullptr);
            if (!p.target.compare_exchange_strong (
                  e, r,
                  memory_order_release,
                  memory_order_consume))
              assert (e == r);
          }
        }

        return r;
      }

    public:
      const file&
      resolve_library (action,
                       const scope&,
                       name,
                       linfo,
                       const dir_paths&,
                       optional<dir_paths>&) const;

      template <typename T>
      static ulock
      insert_library (T*&,
                      const string&,
                      const dir_path&,
                      optional<string>,
                      bool,
                      tracer&);

      target*
      search_library (action,
                      const dir_paths&,
                      optional<dir_paths>&,
                      const prerequisite_key&,
                      bool existing = false) const;

      const target*
      search_library_existing (action a,
                               const dir_paths& sysd,
                               optional<dir_paths>& usrd,
                               const prerequisite_key& pk) const
      {
        return search_library (a, sysd, usrd, pk, true);
      }

      dir_paths
      extract_library_dirs (const scope&) const;

      // Alternative search logic for VC (msvc.cxx).
      //
      bin::liba*
      msvc_search_static (const process_path&,
                          const dir_path&,
                          const prerequisite_key&,
                          bool existing) const;

      bin::libs*
      msvc_search_shared (const process_path&,
                          const dir_path&,
                          const prerequisite_key&,
                          bool existing) const;

      // The pkg-config file searching and loading (pkgconfig.cxx)
      //
      using pkgconfig_callback = function<bool (dir_path&& d)>;

      bool
      pkgconfig_search (const dir_path&, const pkgconfig_callback&) const;

      pair<path, path>
      pkgconfig_search (const dir_path&,
                        const optional<project_name>&,
                        const string&,
                        bool) const;

      void
      pkgconfig_load (action, const scope&,
                      bin::lib&, bin::liba*, bin::libs*,
                      const pair<path, path>&,
                      const dir_path&,
                      const dir_paths&,
                      const dir_paths&) const;

      bool
      pkgconfig_load (action, const scope&,
                      bin::lib&, bin::liba*, bin::libs*,
                      const optional<project_name>&,
                      const string&,
                      const dir_path&,
                      const dir_paths&,
                      const dir_paths&) const;
    };
  }
}

#endif // BUILD2_CC_COMMON_HXX
