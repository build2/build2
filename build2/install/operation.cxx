// file      : build2/install/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/install/operation>

using namespace std;
using namespace butl;

namespace build2
{
  namespace install
  {
    static operation_id
    install_pre (meta_operation_id mo)
    {
      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != disfigure_id ? update_id : 0;
    }

    operation_info install {
      install_id,
      "install",
      "install",
      "installing",
      "has nothing to install", // We cannot "be installed".
      execution_mode::first,
      &install_pre,
      nullptr
    };

    // Note that we run update as a pre-operation, just like install. Which
    // may seem bizarre at first. We do it to obtain the exact same dependency
    // graph as install so that we uninstall exactly the same set of files as
    // install would install. Note that just matching the rules without
    // executing them may not be enough: for example, a presence of an ad hoc
    // group member may only be discovered after executing the rule (e.g., VC
    // link.exe only creates a DLL's import library if there are any exported
    // symbols).
    //
    operation_info uninstall {
      uninstall_id,
      "uninstall",
      "uninstall",
      "uninstalling",
      "is not installed",
      execution_mode::last,
      &install_pre,
      nullptr
    };
  }
}
