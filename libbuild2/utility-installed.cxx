// file      : libbuild2/utility-installed.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

// Bootstrap build is never considered installed.
//
#ifndef BUILD2_BOOTSTRAP

#include <libbuild2/utility.hxx>

namespace build2
{
  const bool build_installed = true;

#ifdef BUILD2_INSTALL_LIB
  const dir_path build_install_lib (BUILD2_INSTALL_LIB);
#endif
}

#endif
