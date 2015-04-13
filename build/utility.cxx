// file      : build/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/utility>

using namespace std;

namespace build
{
  const string empty_string;
  const path empty_path;
  const dir_path empty_dir_path;

  bool exception_unwinding_dtor = false;

  string_pool extension_pool;
}
