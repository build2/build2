// file      : build2/install/operation.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
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
    extern const operation_info install;
    extern const operation_info uninstall;
  }
}

#endif // BUILD2_INSTALL_OPERATION_HXX
