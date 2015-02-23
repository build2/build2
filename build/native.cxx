// file      : build/native.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/native>

using namespace std;

namespace build
{
  const target_type exe::static_type
  {
    typeid (exe),
    "exe",
    &file::static_type,
    &target_factory<exe>,
    file::static_type.search
  };

  const target_type obj::static_type
  {
    typeid (obj),
    "obj",
    &file::static_type,
    &target_factory<obj>,
    file::static_type.search
  };
}
