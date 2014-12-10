// file      : build/native.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/native>

using namespace std;

namespace build
{
  using type_info = target::type_info;

  const type_info exe::ti_ {typeid (exe), "exe", &file::ti_};
  const type_info obj::ti_ {typeid (obj), "obj", &file::ti_};
}
