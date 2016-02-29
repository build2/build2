// file      : build2/search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/search>

#include <butl/filesystem>

#include <build2/scope>
#include <build2/target>
#include <build2/prerequisite>
#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
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
      d = pk.scope->out_path ();

      if (!tk.dir->empty ())
      {
        d /= *tk.dir;
        d.normalize ();
      }
    }

    auto i (targets.find (*tk.type, d, *tk.name, tk.ext, trace));

    if (i == targets.end ())
      return 0;

    target& t (**i);

    l5 ([&]{trace << "existing target " << t << " for prerequisite " << pk;});
    return &t;
  }

  target*
  search_existing_file (const prerequisite_key& cpk, const dir_paths& sp)
  {
    tracer trace ("search_existing_file");

    const target_key& ctk (cpk.tk);
    assert (ctk.dir->relative ());

    // Figure out the extension. Pretty similar logic to file::derive_path().
    //
    const string* ext (ctk.ext);

    if (ext == nullptr)
    {
      if (auto f = ctk.type->extension)
      {
        ext = f (ctk, *cpk.scope); // Already from the pool.
      }

      if (ext == nullptr)
      {
        // What should we do here, fail or say we didn't find anything?
        // Current think is that if the target type couldn't find the default
        // extension, then we simply shouldn't search for any existing files
        // (of course, if the user specified the extension explicitly, we will
        // still do so).
        //
        l4 ([&]{trace << "no existing file for prerequisite " << cpk;});
        return nullptr;
      }
    }

    // Make a copy with the updated extension.
    //
    const prerequisite_key pk {
      cpk.proj, target_key {ctk.type, ctk.dir, ctk.name, ext}, cpk.scope};
    const target_key& tk (pk.tk);

    // Go over paths looking for a file.
    //
    for (const dir_path& d: sp)
    {
      path f (d / *tk.dir / path (*tk.name));
      f.normalize ();

      if (!ext->empty ())
      {
        f += '.';
        f += *ext;
      }

      timestamp mt (file_mtime (f));

      if (mt == timestamp_nonexistent)
        continue;

      l5 ([&]{trace << "found existing file " << f << " for prerequisite "
                    << cpk;});

      // Find or insert. Note: using our updated extension.
      //
      auto r (targets.insert (*tk.type, f.directory (), *tk.name, ext, trace));

      // Has to be a file_target.
      //
      file& t (dynamic_cast<file&> (r.first));

      l5 ([&]{trace << (r.second ? "new" : "existing") << " target " << t
                    << " for prerequisite " << cpk;});

      if (t.path ().empty ())
        t.path (move (f));

      t.mtime (mt);
      return &t;
    }

    l4 ([&]{trace << "no existing file for prerequisite " << cpk;});
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
      d = pk.scope->out_path ();

      if (!tk.dir->empty ())
      {
        d /= *tk.dir;
        d.normalize ();
      }
    }

    // Find or insert.
    //
    auto r (targets.insert (*tk.type, move (d), *tk.name, tk.ext, trace));
    assert (r.second);

    target& t (r.first);

    l5 ([&]{trace << "new target " << t << " for prerequisite " << pk;});
    return t;
  }
}
