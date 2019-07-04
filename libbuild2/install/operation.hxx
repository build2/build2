// file      : libbuild2/install/operation.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_INSTALL_OPERATION_HXX
#define LIBBUILD2_INSTALL_OPERATION_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/operation.hxx>

namespace build2
{
  namespace install
  {
    extern const operation_info op_install;
    extern const operation_info op_uninstall;
    extern const operation_info op_update_for_install;
  }
}

#endif // LIBBUILD2_INSTALL_OPERATION_HXX
