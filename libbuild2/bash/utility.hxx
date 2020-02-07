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
    // Strip the .bash extension from the project name.
    //
    // Note that the result may not be a valid project name.
    //
    inline string
    project_base (const project_name& pn)
    {
      return pn.base ("bash");
    }
  }
}

#endif // LIBBUILD2_BASH_UTILITY_HXX
