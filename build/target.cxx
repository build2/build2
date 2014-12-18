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
    // @@ TODO: need to come up with a relative (to current) path.

    return os << t.type ().name << '{' << t.name << '}';
  }

  target_set targets;
  target* default_target = nullptr;
  target_type_map target_types;

  // path_target
  //
  timestamp path_target::
  load_mtime () const
  {
    assert (!path_.empty ());
    return path_mtime (path_);
  }

  const target_type target::static_type {
    typeid (target), "target", nullptr, nullptr};

  const target_type mtime_target::static_type {
    typeid (mtime_target), "mtime_target", &target::static_type, nullptr};

  const target_type path_target::static_type {
    typeid (path_target), "path_target", &mtime_target::static_type, nullptr};

  const target_type file::static_type {
    typeid (file), "file", &path_target::static_type, &target_factory<file>};
}
