// file      : build/search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/search>

#include <utility>  // move
#include <cassert>

#include <build/path>
#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/timestamp>
#include <build/diagnostics>

using namespace std;

namespace build
{
  target*
  search_existing_target (prerequisite& p)
  {
    tracer trace ("search_existing_target");

    assert (p.target == nullptr);

    // Look for an existing target in this prerequisite's directory scope.
    //
    path d;
    if (p.dir.absolute ())
      d = p.dir; // Already normalized.
    else
    {
      d = p.scope.path ();

      if (!p.dir.empty ())
      {
        d /= p.dir;
        d.normalize ();
      }
    }

    auto i (targets.find (p.type, d, p.name, p.ext, trace));

    if (i == targets.end ())
      return 0;

    target& t (**i);

    level4 ([&]{trace << "existing target " << t << " for prerequsite "
                      << p;});

    p.target = &t;
    return &t;
  }

  target*
  search_existing_file (prerequisite& p, const paths& sp)
  {
    tracer trace ("search_existing_file");

    assert (p.dir.relative ());

    // Go over paths and extension looking for a file.
    //
    for (const path& d: sp)
    {
      path f (d / p.dir / path (p.name));
      f.normalize ();

      // @@ TMP: use target name as an extension.
      //
      const string& e (p.ext != nullptr ? *p.ext : p.type.name);

      if (!e.empty ())
      {
        f += '.';
        f += e;
      }

      timestamp mt (path_mtime (f));

      if (mt == timestamp_nonexistent)
        continue;

      level4 ([&]{trace << "found existing file " << f << " for prerequsite "
                        << p;});

      // Find or insert.
      //
      auto r (targets.insert (p.type, f.directory (), p.name, p.ext, trace));

      // Has to be a path_target.
      //
      path_target& t (dynamic_cast<path_target&> (r.first));

      level4 ([&]{trace << (r.second ? "new" : "existing") << " target "
                        << t << " for prerequsite " << p;});

      t.path (move (f));
      t.mtime (mt);
      p.target = &t;
      return &t;
    }

    return nullptr;
  }

  target&
  create_new_target (prerequisite& p)
  {
    tracer trace ("create_new_target");

    assert (p.target == nullptr);

    // We default to the target in this prerequisite's directory scope.
    //
    path d;
    if (p.dir.absolute ())
      d = p.dir; // Already normalized.
    else
    {
      d = p.scope.path ();

      if (!p.dir.empty ())
      {
        d /= p.dir;
        d.normalize ();
      }
    }

    // Find or insert.
    //
    auto r (targets.insert (p.type, move (d), p.name, p.ext, trace));
    assert (r.second);

    target& t (r.first);

    level4 ([&]{trace << "new target " << t << " for prerequsite " << p;});

    p.target = &t;
    return t;
  }
}
