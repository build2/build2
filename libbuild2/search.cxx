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
  search_existing_target (context& ctx, const prerequisite_key& pk)
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

    const target* t (
      ctx.targets.find (*tk.type, d, o, *tk.name, tk.ext, trace));

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
    // relative, we need to complete it, which is @@ OUT TODO). Note that we
    // blindly trust the user's value which can be used for some interesting
    // tricks, for example:
    //
    // ../cxx{foo}@./
    //
    dir_path out;

    if (tk.out->empty ())
    {
      if (s->out_path () != s->src_path ())
        out = out_src (d, *s->root_scope ());
    }
    else
      out = *tk.out;

    // Find or insert. Note that we are using our updated extension.
    //
    auto r (ctx.targets.insert (*tk.type,
                                move (d),
                                move (out),
                                *tk.name,
                                ext,
                                target_decl::prereq_file,
                                trace));

    // Has to be a file_target.
    //
    const file& t (dynamic_cast<const file&> (r.first));

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
    auto r (ctx.targets.insert (*tk.type,
                                move (d),
                                *tk.out,
                                *tk.name,
                                tk.ext,
                                target_decl::prereq_new,
                                trace));

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
    auto r (ctx.targets.insert_locked (*tk.type,
                                       move (d),
                                       *tk.out,
                                       *tk.name,
                                       tk.ext,
                                       target_decl::prereq_new,
                                       trace));

    // @@ Writing the target (r.first) to the stream ends up in target::ext()
    //    which tries to slock ctx.targets.mutex_, which is already ulock-ed
    //    (r.second) by the current thread. This results with
    //    system_error(errc::resource_deadlock_would_occur).
    //
    //l5 ([&]{trace << (r.second ? "new" : "existing") << " target " << r.first
    //              << " for prerequisite " << pk;});
    return r;
  }
}
