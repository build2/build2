// file      : build/install/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/install/operation>

#include <build/config/operation>

using namespace std;
using namespace butl;

namespace build
{
  namespace install
  {
    static operation_id
    install_pre (meta_operation_id mo)
    {
      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != config::disfigure_id ? update_id : 0;
    }

    operation_info install {
      "install",
      "install",
      "installing",
      "has nothing to install", // We cannot "be installed".
      execution_mode::first,
      &install_pre,
      nullptr
    };
  }
}
