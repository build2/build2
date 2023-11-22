// file      : libbuild2/cc/compile-rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_COMPILE_RULE_HXX
#define LIBBUILD2_CC_COMPILE_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/dyndep.hxx>
#include <libbuild2/file-cache.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/common.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  class depdb;

  namespace cc
  {
    class config_module;

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

    class LIBBUILD2_CC_SYMEXPORT compile_rule: public simple_rule,
                                               virtual common,
                                               dyndep_rule
    {
    public:
      struct match_data;

      compile_rule (data&&, const scope&);

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;

      target_state
      perform_update (action, const target&, match_data&) const;

      target_state
      perform_clean (action, const target&, const target_type&) const;

    public:
      using appended_libraries = small_vector<const target*, 256>;

      void
      append_library_options (appended_libraries&, strings&,
                              const scope&,
                              action, const file&, bool, linfo,
                              bool, bool) const;

      optional<path>
      find_system_header (const path&) const;

    protected:
      static void
      functions (function_family&, const char*); // functions.cxx

    private:
      using environment = small_vector<const char*, 2>;

      template <typename T>
      void
      append_sys_hdr_options (T&) const;

      template <typename T>
      void
      append_library_options (appended_libraries&, T&,
                              const scope&,
                              const scope*,
                              action, const file&, bool, linfo, bool,
                              library_cache*) const;

      template <typename T>
      void
      append_library_options (T&,
                              const scope&,
                              action, const target&, linfo) const;

      using prefix_map = dyndep_rule::prefix_map;
      using srcout_map = dyndep_rule::srcout_map;

      void
      append_prefixes (prefix_map&,
                       const scope&, const target&,
                       const variable&) const;

      void
      append_library_prefixes (appended_libraries&, prefix_map&,
                               const scope&,
                               action, const target&, linfo) const;

      prefix_map
      build_prefix_map (const scope&, action, const target&, linfo) const;

      struct gcc_module_mapper_state;

      optional<bool>
      gcc_module_mapper (gcc_module_mapper_state&,
                         action, const scope&, file&, linfo,
                         const string&, ofdstream&,
                         depdb&, bool&, bool&,
                         optional<prefix_map>&, srcout_map&) const;

      pair<const file*, bool>
      enter_header (action, const scope&, file&, linfo,
                    path&&, bool, bool,
                    optional<prefix_map>&, const srcout_map&) const;

      optional<bool>
      inject_header (action, file&, const file&, timestamp, bool) const;

      void
      extract_headers (action, const scope&, file&, linfo,
                       const file&, match_data&,
                       depdb&, bool&, timestamp, module_imports&,
                       pair<file_cache::entry, bool>&) const;

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

      pair<target&, ulock>
      make_module_sidebuild (action, const scope&,
                             const file*, otype,
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
      const config_module* header_cache_;
    };
  }
}

#endif // LIBBUILD2_CC_COMPILE_RULE_HXX
