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
    inline string
    project_base (const string& n)
    {
      size_t p (path::traits::find_extension (n));
      return p == string::npos || casecmp (n.c_str () + p, ".bash", 5) != 0
        ? n
        : string (n, 0, p);
    }
  }
}

#endif // BUILD2_BASH_UTILITY_HXX
