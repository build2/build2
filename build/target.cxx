// file      : build/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/target>

#include <build/scope>
#include <build/search>
#include <build/context>
#include <build/diagnostics>

using namespace std;

namespace build
{
  // target_type
  //
  const target_type*
  resolve_target_type (const std::string name);

  // target
  //
  ostream&
  operator<< (ostream& os, const target& t)
  {
    os << t.type ().name << '{';

    if (!t.dir.empty ())
    {
      string s (diag_relative_work (t.dir));

      if (s != ".")
      {
        os << s;

        if (!t.name.empty () && s.back () != path::traits::directory_separator)
          os << path::traits::directory_separator;
      }
    }

    os << t.name;

    if (t.ext != nullptr && !t.ext->empty ())
      os << '.' << *t.ext;

    os << '}';

    return os;
  }

  static target*
  search_target (prerequisite& p)
  {
    // The default behavior is to look for an existing target in the
    // prerequisite's directory scope.
    //
    return search_existing_target (p);
  }

  // target_set
  //
  auto target_set::
  find (const key& k, tracer& trace) const -> iterator
  {
    iterator i (map_.find (k));

    if (i != end ())
    {
      target& t (**i);

      // Update the extension if the existing target has it unspecified.
      //
      const string* ext (*k.ext);
      if (t.ext != ext)
      {
        level4 ([&]{
            diag_record r (trace);
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
    }

    return i;
  }

  pair<target&, bool> target_set::
  insert (const target_type& tt,
          path dir,
          std::string name,
          const std::string* ext,
          tracer& trace)
  {
    iterator i (find (key {&tt.id, &dir, &name, &ext}, trace));

    if (i != end ())
      return pair<target&, bool> (**i, false);

    unique_ptr<target> t (tt.factory (move (dir), move (name), ext));
    i = map_.emplace (
      make_pair (key {&tt.id, &t->dir, &t->name, &t->ext},
                 move (t))).first;

    return pair<target&, bool> (**i, true);
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

  // file target
  //

  static target*
  search_file (prerequisite& p)
  {
    // First see if there is an existing target.
    //
    if (target* t = search_existing_target (p))
      return t;

    // Then look for an existing file in this target-type-specific
    // list of paths (@@ TODO: comes from the variable).
    //
    if (p.dir.relative ())
    {
      paths sp;
      sp.push_back (src_out (p.scope.path ())); // src_base

      return search_existing_file (p, sp);
    }
    else
      return nullptr;
  }

  // dir target
  //
  static target*
  search_alias (prerequisite& p)
  {
    // For an alias/action we don't want to silently create a target
    // since it will do nothing and it most likely not what the author
    // intended.
    //
    if (target* t = search_existing_target (p))
      return t;

    fail << "no explicit target for prerequisite " << p;
  }

  // type info
  //

  const target_type target::static_type
  {
    typeid (target),
    "target",
    nullptr,
    nullptr,
    &search_target
  };

  const target_type mtime_target::static_type
  {
    typeid (mtime_target),
    "mtime_target",
    &target::static_type,
    nullptr,
    target::static_type.search
  };

  const target_type path_target::static_type
  {
    typeid (path_target),
    "path_target",
    &mtime_target::static_type,
    nullptr,
    mtime_target::static_type.search
  };

  const target_type file::static_type
  {
    typeid (file),
    "file",
    &path_target::static_type,
    &target_factory<file>,
    &search_file
  };

  const target_type dir::static_type
  {
    typeid (dir),
    "dir",
    &target::static_type,
    &target_factory<dir>,
    &search_alias
  };

  const target_type fsdir::static_type
  {
    typeid (fsdir),
    "fsdir",
    &target::static_type,
    &target_factory<fsdir>,
    target::static_type.search
  };
}
