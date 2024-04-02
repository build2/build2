// file      : libbuild2/search.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/search.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/filesystem.hxx>   // mtime()
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite-key.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  const target*
  search_existing_target (context& ctx,
                          const prerequisite_key& pk,
                          bool out_only)
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
    // empty    This means out is undetermined and we search for a target
    //          first in the out tree (which happens to be indicated by an
    //          empty value, so we can just pass this as is) and if not
    //          found, then in the src tree (unless suppressed).
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
        if (!tk.out->current ())
        {
          o /= *tk.out;
          o.normalize ();
        }
      }

      // Drop out if it is the same as src (in-src build).
      //
      if (o == d)
        o.clear ();
    }

    const target* t (
      ctx.targets.find (*tk.type, d, o, *tk.name, tk.ext, trace));

    // Try in the src tree.
    //
    if (t == nullptr             &&
        !out_only                &&
        tk.out->empty ()         &&
        tk.dir->relative ()      &&
        !pk.scope->out_eq_src ())
    {
      o = move (d);

      d = pk.scope->src_path ();

      if (!tk.dir->empty ())
      {
        d /= *tk.dir;
        d.normalize ();
      }

      t = ctx.targets.find (*tk.type, d, o, *tk.name, tk.ext, trace);
    }

    if (t != nullptr)
      l5 ([&]{trace << "existing target " << *t
                    << " for prerequisite " << pk;});

    return t;
  }

  const target*
  search_existing_file (context& ctx, const prerequisite_key& cpk)
  {
    tracer trace ("search_existing_file");

    const target_key& ctk (cpk.tk);
    const scope* s (cpk.scope);

    // Has to be a file target.
    //
    assert (ctk.type->is_a<file> ());

    path f;

    if (ctk.dir->absolute ())
      f = *ctk.dir; // Already normalized.
    else
    {
      f = s->src_path ();

      if (!ctk.dir->empty ())
      {
        f /= *ctk.dir;
        f.normalize ();
      }
    }

    // Bail out if not inside project's src_root.
    //
    if (s == nullptr || !f.sub (s->root_scope ()->src_path ()))
      return nullptr;

    // Figure out the extension. Pretty similar logic to file::derive_path().
    //
    optional<string> ext (ctk.ext);

    if (!ext)
    {
      if (auto f = ctk.type->fixed_extension)
        ext = f (ctk, s->root_scope ());
      else if (auto f = ctk.type->default_extension)
        ext = f (ctk, *s, nullptr, true);

      if (!ext)
      {
        // What should we do here, fail or say we didn't find anything?
        // Current think is that if the target type couldn't find the default
        // extension, then we simply shouldn't search for any existing files
        // (of course, if the user specified the extension explicitly, we will
        // still do so).
        //
        l4 ([&]{trace << "no default extension for prerequisite " << cpk;});
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
    f /= *tk.name;

    if (!ext->empty ())
    {
      f += '.';
      f += *ext;
    }

    timestamp mt (mtime (f));

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
    // relative, we need to complete it).
    //
    dir_path out;

    if (tk.out->empty ())
    {
      if (!s->out_eq_src ())
        out = out_src (d, *s->root_scope ());
    }
    else
    {
      if (tk.out->absolute ())
        out = *tk.out; // Already normalized.
      else
      {
        out = pk.scope->out_path ();
        if (!tk.out->current ())
        {
          out /= *tk.out;
          out.normalize ();
        }
      }

      // Drop out if it is the same as src (in-src build).
      //
      if (out == d)
        out.clear ();
    }

    // Find or insert. Note that we are using our updated extension.
    //
    // More often insert than find, so skip find in insert().
    //
    auto r (ctx.targets.insert (*tk.type,
                                move (d),
                                move (out),
                                *tk.name,
                                ext,
                                target_decl::prereq_file,
                                trace,
                                true /* skip_find */));

    const file& t (r.first.as<file> ());

    l5 ([&]{trace << (r.second ? "new" : "existing") << " target " << t
                  << " for prerequisite " << cpk;});

    t.path_mtime (move (f), mt);

    return &t;
  }

  const target&
  create_new_target (context& ctx, const prerequisite_key& pk)
  {
    tracer trace ("create_new_target");

    const target_key& tk (pk.tk);

    // If out is present, then it means the target is in src and we shouldn't
    // be creating new targets in src, should we? Feels like this should not
    // even be called if out is not empty.
    //
    assert (tk.out->empty ());

    // We default to the target in this directory scope.
    //
    dir_path d;
    if (tk.dir->absolute ())
    {
      d = *tk.dir; // Already normalized.

      // Even if out is empty, it may still be (only) in src.
      //
      // Note: issue diagnostics consistent with search() after skipping this
      // function due to non-empty out.
      //
      // @@ PERF: we could first check if it's in pk.scope, which feels like
      //          the common case. Though this doesn't seem to affect
      //          performance in any noticeable way.
      //
      auto p (ctx.scopes.find (d, false)); // Note: never empty.
      if (*p.first == nullptr && ++p.first != p.second)
      {
        fail << "no existing source file for prerequisite " << pk << endf;
      }
    }
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
    // More often insert than find, so skip find in insert().
    //
    auto r (ctx.targets.insert (*tk.type,
                                move (d),
                                *tk.out,
                                *tk.name,
                                tk.ext,
                                target_decl::prereq_new,
                                trace,
                                true /* skip_find */));

    const target& t (r.first);
    l5 ([&]{trace << (r.second ? "new" : "existing") << " target " << t
                  << " for prerequisite " << pk;});
    return t;
  }

  pair<target&, ulock>
  create_new_target_locked (context& ctx, const prerequisite_key& pk)
  {
    tracer trace ("create_new_target_locked");

    const target_key& tk (pk.tk);

    // If out is present, then it means the target is in src and we shouldn't
    // be creating new targets in src, should we? Feels like this should not
    // even be called if out is not empty.
    //
    assert (tk.out->empty ());

    // We default to the target in this directory scope.
    //
    dir_path d;
    if (tk.dir->absolute ())
    {
      d = *tk.dir; // Already normalized.

      // As above.
      //
      auto p (ctx.scopes.find (d, false));
      if (*p.first == nullptr && ++p.first != p.second)
      {
        fail << "no existing source file for prerequisite " << pk << endf;
      }
    }
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
    // More often insert than find, so skip find in insert_locked().
    //
    auto r (ctx.targets.insert_locked (*tk.type,
                                       move (d),
                                       *tk.out,
                                       *tk.name,
                                       tk.ext,
                                       target_decl::prereq_new,
                                       trace,
                                       true /* skip_find */));
    l5 ([&]
        {
          diag_record dr (trace);
          if (r.second)
            dr << "new target " << r.first.key_locked ();
          else
            dr << "existing target " << r.first;
          dr << " for prerequisite " << pk;
        });

    return r;
  }
}
