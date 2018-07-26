// file      : build2/bash/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_BASH_UTILITY_HXX
#define BUILD2_BASH_UTILITY_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

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

#endif // BUILD2_BASH_UTILITY_HXX
