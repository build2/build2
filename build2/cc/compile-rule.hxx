// file      : build2/cc/compile-rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_COMPILE_RULE_HXX
#define BUILD2_CC_COMPILE_RULE_HXX

#include <libbutl/path-map.mxx>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>
#include <build2/filesystem.hxx> // auto_rmfile

#include <build2/cc/types.hxx>
#include <build2/cc/common.hxx>

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

    class compile_rule: public rule, virtual common
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

    private:
      struct match_data;
      using environment = small_vector<const char*, 2>;

      void
      append_lib_options (const scope&,
                          cstrings&,
                          action,
                          const target&,
                          linfo) const;

      void
      hash_lib_options (const scope&,
                        sha256&,
                        action,
                        const target&,
                        linfo) const;

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
      using prefix_map = butl::dir_path_map<prefix_value>;

      void
      append_prefixes (prefix_map&, const target&, const variable&) const;

      void
      append_lib_prefixes (const scope&,
                           prefix_map&,
                           action,
                           target&,
                           linfo) const;

      prefix_map
      build_prefix_map (const scope&, action, target&, linfo) const;

      // Reverse-lookup target type from extension.
      //
      const target_type*
      map_extension (const scope&, const string&, const string&) const;

      pair<auto_rmfile, bool>
      extract_headers (action, const scope&, file&, linfo,
                       const file&, const match_data&,
                       depdb&, bool&, timestamp) const;

      pair<translation_unit, string>
      parse_unit (action, file&, linfo,
                  const file&, auto_rmfile&, const match_data&) const;

      void
      extract_modules (action, const scope&, file&, linfo,
                       const compile_target_types&,
                       const file&, match_data&,
                       module_info&&, depdb&, bool&) const;

      module_positions
      search_modules (action, const scope&, file&, linfo,
                      const target_type&,
                      const file&, module_imports&, sha256&) const;

      const target&
      make_module_sidebuild (action, const scope&, const target&,
                             const target&, const string&) const;

      void
      append_modules (environment&, cstrings&, strings&,
                      action, const file&, const match_data&) const;

      // Language selection option (for VC) or the value for the -x option.
      //
      const char*
      langopt (const match_data&) const;

      void
      append_symexport_options (cstrings&, const target&) const;

    private:
      const string rule_id;
    };
  }
}

#endif // BUILD2_CC_COMPILE_RULE_HXX
