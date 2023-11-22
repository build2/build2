// file      : libbuild2/utility-uninstalled.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/utility.hxx>

namespace build2
{
  const bool build_installed = false;
  const dir_path build_install_lib; // Empty.

#ifdef BUILD2_INSTALL_BUILDFILE
  const dir_path build_install_buildfile (BUILD2_INSTALL_BUILDFILE);
#else
  const dir_path build_install_buildfile; // Empty.
#endif

#ifdef BUILD2_INSTALL_DATA
  const dir_path build_install_data (BUILD2_INSTALL_DATA);
#else
  const dir_path build_install_data; // Empty (during bootstrap).
#endif
}
