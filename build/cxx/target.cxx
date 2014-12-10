// file      : build/cxx/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/target>

using namespace std;

namespace build
{
  namespace cxx
  {
    using type_info = target::type_info;

    const type_info hxx::ti_ {typeid (hxx), "hxx", &file::ti_};
    const type_info ixx::ti_ {typeid (ixx), "ixx", &file::ti_};
    const type_info txx::ti_ {typeid (txx), "txx", &file::ti_};
    const type_info cxx::ti_ {typeid (cxx), "cxx", &file::ti_};
  }
}
