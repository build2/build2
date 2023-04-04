// file      : libbuild2/install/operation.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_INSTALL_OPERATION_HXX
#define LIBBUILD2_INSTALL_OPERATION_HXX

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/serializer.hxx>
#endif

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/operation.hxx>
#include <libbuild2/filesystem.hxx> // auto_rmfile

namespace build2
{
  namespace install
  {
    extern const operation_info op_install;
    extern const operation_info op_uninstall;
    extern const operation_info op_update_for_install;

    // Set as context::current_inner_odata during the install/uninstall inner
    // operations.
    //
    struct context_data
    {
      // Manifest.
      //
#ifndef BUILD2_BOOTSTRAP
      path      manifest_file; // Absolute and normalized, empty if `-`.
      path_name manifest_name; // Original path/name.
      ofdstream manifest_ofs;
      ostream&  manifest_os;
      auto_rmfile manifest_autorm;
      butl::json::stream_serializer manifest_json;
      const target* manifest_target = nullptr; // Target being installed.
      struct manifest_target_entry
      {
        build2::path path;
        string       mode;
        build2::path target;
      };
      vector<manifest_target_entry> manifest_target_entries;
#endif

      // The following manifest_install_[dfl]() functions correspond to (and
      // are called from) file_rule::install_[dfl]().

      // install -d -m <mode> <dir>
      //
      static void
      manifest_install_d (context&,
                          const target&,
                          const dir_path& dir,
                          const string& mode);

      // install -m <mode> <file> <dir>/<name>
      //
      static void
      manifest_install_f (context&,
                          const target& file,
                          const dir_path& dir,
                          const path& name,
                          const string& mode);

      // install -l <link_target> <dir>/<link>
      //
      static void
      manifest_install_l (context&,
                          const target&,
                          const path& link_target,
                          const dir_path& dir,
                          const path& link);

      // Constructor.
      //
      explicit
      context_data (const path* manifest);
    };
  }
}

#endif // LIBBUILD2_INSTALL_OPERATION_HXX
