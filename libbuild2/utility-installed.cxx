// file      : libbuild2/utility-installed.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

// Bootstrap build is never condidered installed.
//
#ifndef BUILD2_BOOTSTRAP

#include <libbuild2/utility.hxx>

namespace build2
{
  struct build_installed_init
  {
    build_installed_init ()
    {
      build_installed = true;
    }
  };

  static build_installed_init init_;
}

#endif
