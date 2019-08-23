// file      : libbuild2/install/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/operation.hxx>

#include <libbuild2/variable.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace install
  {
    static operation_id
    install_pre (const values& params, meta_operation_id mo, const location& l)
    {
      if (!params.empty ())
        fail (l) << "unexpected parameters for operation install";

      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != disfigure_id ? update_id : 0;
    }

    // Note that we run both install and uninstall serially. The reason for
    // this is all the fuzzy things we are trying to do like removing empty
    // outer directories if they are empty. If we do this in parallel, then
    // those things get racy. Also, since all we do here is creating/removing
    // files, there is not going to be much speedup from doing it in parallel.

    const operation_info op_install {
      install_id,
      0,
      "install",
      "install",
      "installing",
      "installed",
      "has nothing to install", // We cannot "be installed".
      execution_mode::first,
      0,
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
    const operation_info op_uninstall {
      uninstall_id,
      0,
      "uninstall",
      "uninstall",
      "uninstalling",
      "uninstalled",
      "is not installed",
      execution_mode::last,
      0,
      &install_pre,
      nullptr
    };

    // Also the explicit update-for-install operation alias.
    //
    const operation_info op_update_for_install {
      update_id, // Note: not update_for_install_id.
      install_id,
      op_update.name,
      op_update.name_do,
      op_update.name_doing,
      op_update.name_did,
      op_update.name_done,
      op_update.mode,
      op_update.concurrency,
      op_update.pre,
      op_update.post
    };
  }
}
