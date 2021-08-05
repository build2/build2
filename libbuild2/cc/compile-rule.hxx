// file      : libbuild2/cc/compile-rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_COMPILE_RULE_HXX
#define LIBBUILD2_CC_COMPILE_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/file-cache.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/common.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  class depdb;

  namespace cc
  {
    // The order is arranged so that their integral values indicate whether
    // one is a "stronger" than another.
    //
    enum class preprocessed: uint8_t {none, includes, modules, all};

    // Positions of the re-exported bmi{}s. See search_modules() for
    // details.
    //
    struct module_positions
    {
      size_t start;    // First imported    bmi*{}, 0 if none.
      size_t exported; // First re-exported bmi*{}, 0 if none.
      size_t copied;   // First copied-over bmi*{}, 0 if none.
    };

    class LIBBUILD2_CC_SYMEXPORT compile_rule: public simple_rule, virtual common
    {
    public:
      compile_rule (data&&);

      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      target_state
      perform_update (action, const target&) const;

      target_state
      perform_clean (action, const target&) const;

    public:
      using appended_libraries = small_vector<const file*, 256>;

      void
      append_library_options (appended_libraries&, strings&,
                              const scope&,
                              action, const file&, bool, linfo) const;

      optional<path>
      find_system_header (const path&) const;

    protected:
      static void
      functions (function_family&, const char*); // functions.cxx

    private:
      struct match_data;
      using environment = small_vector<const char*, 2>;

      template <typename T>
      void
      append_sys_hdr_options (T&) const;

      template <typename T>
      void
      append_library_options (appended_libraries&, T&,
                              const scope&,
                              action, const file&, bool, linfo,
                              library_cache*) const;

      template <typename T>
      void
      append_library_options (T&,
                              const scope&,
                              action, const target&, linfo) const;

      // Mapping of include prefixes (e.g., foo in <foo/bar>) for auto-
      // generated headers to directories where they will be generated.
      //
      // We are using a prefix map of directories (dir_path_map) instead of
      // just a map in order to also cover sub-paths (e.g., <foo/more/bar> if
      // we continue with the example). Specifically, we need to make sure we
      // don't treat foobar as a sub-directory of foo.
      //
      // The priority is used to decide who should override whom. Lesser
      // values are considered higher priority. See append_prefixes() for
      // details.
      //
      // @@ The keys should be normalized.
      //
      struct prefix_value
      {
        dir_path directory;
        size_t priority;
      };
      using prefix_map = dir_path_map<prefix_value>;

      void
      append_prefixes (prefix_map&, const target&, const variable&) const;

      void
      append_library_prefixes (prefix_map&,
                               const scope&,
                               action, target&, linfo) const;

      prefix_map
      build_prefix_map (const scope&, action, target&, linfo) const;

      small_vector<const target_type*, 2>
      map_extension (const scope&, const string&, const string&) const;

      // Src-to-out re-mapping. See extract_headers() for details.
      //
      using srcout_map = path_map<dir_path>;

      struct module_mapper_state;

      bool
      gcc_module_mapper (module_mapper_state&,
                         action, const scope&, file&, linfo,
                         ifdstream&, ofdstream&,
                         depdb&, bool&, bool&,
                         optional<prefix_map>&, srcout_map&) const;

      pair<const file*, bool>
      enter_header (action, const scope&, file&, linfo,
                    path&&, bool, bool,
                    optional<prefix_map>&, srcout_map&) const;

      optional<bool>
      inject_header (action, file&, const file&, timestamp, bool) const;

      pair<file_cache::entry, bool>
      extract_headers (action, const scope&, file&, linfo,
                       const file&, match_data&,
                       depdb&, bool&, timestamp, module_imports&) const;

      string
      parse_unit (action, file&, linfo,
                  const file&, file_cache::entry&,
                  const match_data&, const path&,
                  unit&) const;

      void
      extract_modules (action, const scope&, file&, linfo,
                       const compile_target_types&,
                       const file&, match_data&,
                       module_info&&, depdb&, bool&) const;

      module_positions
      search_modules (action, const scope&, file&, linfo,
                      const target_type&,
                      const file&, module_imports&, sha256&) const;

      pair<dir_path, const scope&>
      find_modules_sidebuild (const scope&) const;

      const file&
      make_module_sidebuild (action, const scope&, const file&,
                             const target&, const string&) const;

      const file&
      make_header_sidebuild (action, const scope&, const file&,
                             linfo, const file&) const;

      void
      append_header_options (environment&, cstrings&, small_vector<string, 2>&,
                             action, const file&,
                             const match_data&, const path&) const;

      void
      append_module_options (environment&, cstrings&, small_vector<string, 2>&,
                             action, const file&,
                             const match_data&, const path&) const;

      // Compiler-specific language selection options. Return the number of
      // options (arguments, really) appended.
      //
      size_t
      append_lang_options (cstrings&, const match_data&) const;

      void
      append_symexport_options (cstrings&, const target&) const;

    private:
      const string rule_id;
    };
  }
}

#endif // LIBBUILD2_CC_COMPILE_RULE_HXX
