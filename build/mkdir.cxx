// file      : build/mkdir.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/mkdir>

#include <sys/stat.h> // mkdir()

#include <system_error>

using namespace std;

namespace build
{
  void
  mkdir (const path& p, mode_t m)
  {
    if (::mkdir (p.string ().c_str (), m) != 0)
      throw system_error (errno, system_category ());
  }
}
