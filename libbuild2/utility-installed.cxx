// file      : libbuild2/utility-installed.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

// Bootstrap build is never condidered installed.
//
#ifndef BUILD2_BOOTSTRAP

#include <libbuild2/utility.hxx>

namespace build2
{
  bool build_installed = true;
}

#endif
