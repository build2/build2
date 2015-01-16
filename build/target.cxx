// file      : build/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/target>

#include <build/context>
#include <build/diagnostics>

using namespace std;

namespace build
{
  // target
  //
  ostream&
  operator<< (ostream& os, const target& t)
  {
    os << t.type ().name << '{';

    if (!t.directory.empty ())
    {
      string s (diagnostic_string (t.directory));

      if (!s.empty ())
        os << s << path::traits::directory_separator;
    }

    os << t.name;

    if (t.ext != nullptr)
      os << '.' << *t.ext;

    os << '}';

    return os;
  }

  // target_set
  //
  auto target_set::
  insert (const target_type& tt,
          path dir,
          std::string name,
          const std::string* ext,
          tracer& tr) -> pair<target&, bool>
  {
    //@@ OPT: would be nice to somehow first check if this target is
    //   already in the set before allocating a new instance.

    // Find or insert.
    //
    auto r (
      emplace (
        unique_ptr<target> (tt.factory (move (dir), move (name), ext))));

    target& t (**r.first);

    // Update the extension if the existing target has it unspecified.
    //
    if (t.ext != ext)
    {
      trace (4, [&]{
          tracer::record r (tr);
          r << "assuming target " << t << " is the same as the one with ";
          if (ext == nullptr)
            r << "unspecified extension";
          else if (ext->empty ())
            r << "no extension";
          else
            r << "extension " << *ext;
        });

      if (ext != nullptr)
        t.ext = ext;
    }

    return pair<target&, bool> (t, r.second);
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
