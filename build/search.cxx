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
  search_existing_target (const prerequisite_key& pk)
  {
    tracer trace ("search_existing_target");

    const target_key& tk (pk.tk);

    // Look for an existing target in this directory scope.
    //
    dir_path d;
    if (tk.dir->absolute ())
      d = *tk.dir; // Already normalized.
    else
    {
      d = pk.scope->path ();

      if (!tk.dir->empty ())
      {
        d /= *tk.dir;
        d.normalize ();
      }
    }

    auto i (targets.find (*tk.type, d, *tk.name, *tk.ext, trace));

    if (i == targets.end ())
      return 0;

    target& t (**i);

    level4 ([&]{trace << "existing target " << t << " for prerequsite "
                      << pk;});

    return &t;
  }

  target*
  search_existing_file (const prerequisite_key& pk, const dir_paths& sp)
  {
    tracer trace ("search_existing_file");

    const target_key& tk (pk.tk);
    assert (tk.dir->relative ());

    // Go over paths and extension looking for a file.
    //
    for (const dir_path& d: sp)
    {
      path f (d / *tk.dir / path (*tk.name));
      f.normalize ();

      // @@ TMP: use target name as an extension.
      //
      const string& e (*tk.ext != nullptr ? **tk.ext : tk.type->name);

      if (!e.empty ())
      {
        f += '.';
        f += e;
      }

      timestamp mt (path_mtime (f));

      if (mt == timestamp_nonexistent)
        continue;

      level4 ([&]{trace << "found existing file " << f << " for prerequsite "
                        << pk;});

      // Find or insert.
      //
      auto r (
        targets.insert (
          *tk.type, f.directory (), *tk.name, *tk.ext, trace));

      // Has to be a path_target.
      //
      path_target& t (dynamic_cast<path_target&> (r.first));

      level4 ([&]{trace << (r.second ? "new" : "existing") << " target "
                        << t << " for prerequsite " << pk;});

      if (t.path ().empty ())
        t.path (move (f));

      t.mtime (mt);
      return &t;
    }

    return nullptr;
  }

  target&
  create_new_target (const prerequisite_key& pk)
  {
    tracer trace ("create_new_target");

    const target_key& tk (pk.tk);

    // We default to the target in this directory scope.
    //
    dir_path d;
    if (tk.dir->absolute ())
      d = *tk.dir; // Already normalized.
    else
    {
      d = pk.scope->path ();

      if (!tk.dir->empty ())
      {
        d /= *tk.dir;
        d.normalize ();
      }
    }

    // Find or insert.
    //
    auto r (targets.insert (*tk.type, move (d), *tk.name, *tk.ext, trace));
    assert (r.second);

    target& t (r.first);

    level4 ([&]{trace << "new target " << t << " for prerequsite " << pk;});

    return t;
  }
}
