// file      : libbuild2/utility-installed.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

// Bootstrap build is never considered installed.
//
#ifndef BUILD2_BOOTSTRAP

#include <libbuild2/utility.hxx>

namespace build2
{
  const bool build_installed = true;

#ifdef BUILD2_INSTALL_LIB
  dir_path build_install_lib (BUILD2_INSTALL_LIB);
#endif

#ifdef BUILD2_INSTALL_BUILDFILE
  dir_path build_install_buildfile (BUILD2_INSTALL_BUILDFILE);
#endif

#ifdef BUILD2_INSTALL_DATA
  dir_path build_install_data (BUILD2_INSTALL_DATA);
#endif

#ifdef BUILD2_INSTALL_ROOT
  const dir_path build_install_root (BUILD2_INSTALL_ROOT);
#else
  const dir_path build_install_root; // Empty (not relocatable).
#endif

#ifdef BUILD2_INSTALL_ROOT_RELATIVE
  const dir_path build_install_root_relative (BUILD2_INSTALL_ROOT_RELATIVE);
#else
  const dir_path build_install_root_relative; // Empty (not relocatable).
#endif
}

#endif
