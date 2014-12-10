// file      : build/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/target>

#include <ostream>

using namespace std;

namespace build
{
  // target
  //
  ostream&
  operator<< (ostream& os, const target& t)
  {
    return os << t.type_id ().name << '{' << t.name () << '}';
  }

  // path_target
  //
  timestamp path_target::
  load_mtime () const
  {
    assert (!path_.empty ());
    return path_mtime (path_);
  }

  using type_info = target::type_info;

  const type_info target::ti_ {typeid (target), "target", nullptr};
  const type_info mtime_target::ti_ {
    typeid (mtime_target), "mtime_target", &target::ti_};
  const type_info path_target::ti_ {
    typeid (path_target), "path_target", &mtime_target::ti_};
  const type_info file::ti_ {typeid (file), "file", &path_target::ti_};
}
