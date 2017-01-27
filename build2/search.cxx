// file      : build2/search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/search>

#include <butl/filesystem> // file_mtime()

#include <build2/scope>
#include <build2/target>
#include <build2/context>
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

    // Look for an existing target in the prerequisite's scope.
    //
    dir_path d;
    if (tk.dir->absolute ())
      d = *tk.dir; // Already normalized.
    else
    {
      d = tk.out->empty () ? pk.scope->out_path () : pk.scope->src_path ();

      if (!tk.dir->empty ())
      {
        d /= *tk.dir;
        d.normalize ();
      }
    }

    // Prerequisite's out directory can be one of the following:
    //
    // empty    This means out is undetermined and we simply search for a
    //          target that is in the out tree which happens to be indicated
    //          by an empty value, so we can just pass this as is.
    //
    // absolute This is the "final" value that doesn't require any processing
    //          and we simply use it as is.
    //
    // relative The out directory was specified using @-syntax as relative (to
    //          the prerequisite's scope) and we need to complete it similar
    //          to how we complete the relative dir above.
    //
    dir_path o;
    if (!tk.out->empty ())
    {
      if (tk.out->absolute ())
        o = *tk.out; // Already normalized.
      else
      {
        o = pk.scope->out_path ();
        o /= *tk.out;
        o.normalize ();
      }

      // Drop out if it is the same as src (in-src build).
      //
      if (o == d)
        o.clear ();
    }

    auto i (targets.find (*tk.type, d, o, *tk.name, tk.ext, trace));

    if (i == targets.end ())
      return 0;

    target& t (**i);

    l5 ([&]{trace << "existing target " << t << " for prerequisite " << pk;});
    return &t;
  }

  target*
  search_existing_file (const prerequisite_key& cpk)
  {
    tracer trace ("search_existing_file");

    const target_key& ctk (cpk.tk);
    assert (ctk.dir->relative ());

    // Figure out the extension. Pretty similar logic to file::derive_path().
    //
    optional<string> ext (ctk.ext);

    if (!ext)
    {
      if (auto f = ctk.type->extension)
        ext = f (ctk, *cpk.scope, true);

      if (!ext)
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
      cpk.proj, {ctk.type, ctk.dir, ctk.out, ctk.name, ext}, cpk.scope};
    const target_key& tk (pk.tk);

    // Check if there is a file.
    //
    const dir_path& s (pk.scope->src_path ());

    path f (s);
    if (!tk.dir->empty ())
    {
      f /= *tk.dir;
      f.normalize ();
    }
    f /= *tk.name;

    if (!ext->empty ())
    {
      f += '.';
      f += *ext;
    }

    timestamp mt (file_mtime (f));

    if (mt == timestamp_nonexistent)
    {
      l4 ([&]{trace << "no existing file for prerequisite " << cpk;});
      return nullptr;
    }

    l5 ([&]{trace << "found existing file " << f << " for prerequisite "
                  << cpk;});

    dir_path d (f.directory ());

    // Calculate the corresponding out. We have the same three options for the
    // prerequisite's out directory as in search_existing_target(). If it is
    // empty (undetermined), then we need to calculate it since this target
    // will be from the src tree.
    //
    // In the other two cases we use the prerequisite's out (in case it is
    // relative, we need to complete it, which is @@ OUT TODO). Note that we
    // blindly trust the user's value which can be use for some interesting
    // tricks, for example:
    //
    // ../cxx{foo}@./
    //
    dir_path out;

    if (tk.out->empty ())
    {
      if (pk.scope->out_path () != s)
        out = out_src (d, *pk.scope->root_scope ());
    }
    else
      out = *tk.out;

    // Find or insert. Note that we are using our updated extension.
    //
    auto r (targets.insert (
              *tk.type, move (d), move (out), *tk.name, ext, false, trace));

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
    // @@ OUT: same story as in search_existing_target() re out.
    //
    auto r (targets.insert (
              *tk.type, move (d), *tk.out, *tk.name, tk.ext, false, trace));
    assert (r.second);

    target& t (r.first);

    l5 ([&]{trace << "new target " << t << " for prerequisite " << pk;});
    return t;
  }
}
