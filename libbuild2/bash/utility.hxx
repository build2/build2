// file      : libbuild2/bash/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BASH_UTILITY_HXX
#define LIBBUILD2_BASH_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

namespace build2
{
  namespace bash
  {
    // Return the bash{} modules installation directory under bin/.
    //
    // Note that we used to install into bin/<project>/ but that has a good
    // chance of clashing with the project's executable. Adding the .bash
    // extension feels like a good idea since in our model the executables
    // should not use the .bash extension (only modules) and therefore are
    // unlikely to clash with this name.
    //
    // One drawback of this approach is that in case of a project like
    // libbutl.bash we now have different module directories inside the
    // project (libbutl/) and when installed (libbutl.bash/). Also, the
    // installation directory will be shared with the libbutl project but
    // that's probably ok (and we had the same issue before).
    //
    inline string
    modules_install_dir (const project_name& pn)
    {
      // Strip the .bash extension if present not to duplicate it.
      //
      return pn.base ("bash") + ".bash";
    }
  }
}

#endif // LIBBUILD2_BASH_UTILITY_HXX
