// file      : build2/install/operation.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_INSTALL_OPERATION_HXX
#define BUILD2_INSTALL_OPERATION_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/operation.hxx>

namespace build2
{
  namespace install
  {
    extern const operation_info op_install;
    extern const operation_info op_uninstall;
    extern const operation_info op_update_for_install;
  }
}

#endif // BUILD2_INSTALL_OPERATION_HXX
