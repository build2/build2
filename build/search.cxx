// file      : build/search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/search>

#include <utility>  // move()
#include <cassert>

#include <butl/filesystem>

#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/diagnostics>

using namespace std;
using namespace butl;

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
      d = pk.scope->out_path ();

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

    level5 ([&]{trace << "existing target " << t << " for prerequisite "
                      << pk;});

    return &t;
  }

  target*
  search_existing_file (const prerequisite_key& cpk, const dir_paths& sp)
  {
    tracer trace ("search_existing_file");

    prerequisite_key pk (cpk); // Make a copy so we can update extension.
    target_key& tk (pk.tk);
    assert (tk.dir->relative ());

    // Figure out the extension. Pretty similar logic to file::derive_path().
    //
    const string* ext (*tk.ext);

    if (ext == nullptr)
    {
      if (auto f = tk.type->extension)
      {
        ext = &f (tk, *pk.scope); // Already from the pool.
        tk.ext = &ext;
      }
      else
      {
        // What should we do here, fail or say we didn't find anything?
        // Current think is that if the target type didn't provide the
        // default extension, then it doesn't want us to search for an
        // existing file (of course, if the user specified the extension
        // explicitly, we will still do so). But let me know what you
        // think.
        //
        //fail << "no default extension for prerequisite " << pk;
        level4 ([&]{trace << "no existing file found for prerequisite "
                          << pk;});
        return nullptr;
      }
    }

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

      level5 ([&]{trace << "found existing file " << f << " for prerequisite "
                        << pk;});

      // Find or insert. Note: using our updated extension.
      //
      auto r (targets.insert (*tk.type, f.directory (), *tk.name, ext, trace));

      // Has to be a file_target.
      //
      file& t (dynamic_cast<file&> (r.first));

      level5 ([&]{trace << (r.second ? "new" : "existing") << " target "
                        << t << " for prerequisite " << pk;});

      if (t.path ().empty ())
        t.path (move (f));

      t.mtime (mt);
      return &t;
    }

    level4 ([&]{trace << "no existing file found for prerequisite " << pk;});
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
    auto r (targets.insert (*tk.type, move (d), *tk.name, *tk.ext, trace));
    assert (r.second);

    target& t (r.first);

    level5 ([&]{trace << "new target " << t << " for prerequisite " << pk;});

    return t;
  }
}
