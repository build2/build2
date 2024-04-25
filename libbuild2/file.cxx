// file      : libbuild2/file.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/file.hxx>

#include <cerrno>
#include <cstring> // strlen()
#include <iomanip> // left, setw()
#include <sstream>

#include <libbuild2/rule.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite-key.hxx>

#include <libbuild2/token.hxx>
#include <libbuild2/lexer.hxx>
#include <libbuild2/parser.hxx>

#include <libbuild2/config/module.hxx>  // config::module::version
#include <libbuild2/config/utility.hxx> // config::{lookup_*, save_*}()

using namespace std;
using namespace butl;

namespace build2
{
  // Standard and alternative build file/directory naming schemes.
  //
  extern const dir_path std_export_dir;
  extern const dir_path alt_export_dir;

  // build:

  const dir_path std_build_dir       ("build");
  const dir_path std_root_dir        (dir_path (std_build_dir) /= "root");
  const dir_path std_bootstrap_dir   (dir_path (std_build_dir) /= "bootstrap");
  const dir_path std_build_build_dir (dir_path (std_build_dir) /= "build");
  const dir_path std_export_dir      (dir_path (std_build_dir) /= "export");

  const path std_root_file      (std_build_dir     / "root.build");
  const path std_bootstrap_file (std_build_dir     / "bootstrap.build");
  const path std_src_root_file  (std_bootstrap_dir / "src-root.build");
  const path std_out_root_file  (std_bootstrap_dir / "out-root.build");
  const path std_export_file    (std_build_dir     / "export.build");

  const string std_build_ext        ("build");
  const path   std_buildfile_file   ("buildfile");
  const path   std_buildignore_file (".buildignore");

  // build2:

  const dir_path alt_build_dir       ("build2");
  const dir_path alt_root_dir        (dir_path (alt_build_dir) /= "root");
  const dir_path alt_bootstrap_dir   (dir_path (alt_build_dir) /= "bootstrap");
  const dir_path alt_build_build_dir (dir_path (alt_build_dir) /= "build");
  const dir_path alt_export_dir      (dir_path (alt_build_dir) /= "export");

  const path alt_root_file      (alt_build_dir     / "root.build2");
  const path alt_bootstrap_file (alt_build_dir     / "bootstrap.build2");
  const path alt_src_root_file  (alt_bootstrap_dir / "src-root.build2");
  const path alt_out_root_file  (alt_bootstrap_dir / "out-root.build2");
  const path alt_export_file    (alt_build_dir     / "export.build2");

  const string alt_build_ext        ("build2");
  const path   alt_buildfile_file   ("build2file");
  const path   alt_buildignore_file (".build2ignore");

  // Check if the standard/alternative file/directory exists, returning empty
  // path if it does not.
  //
  template <typename T>
  static T
  exists (const dir_path& d, const T& s, const T& a, optional<bool>& altn)
  {
    T p;
    bool e;

    if (altn)
    {
      p = d / (*altn ? a : s);
      e = exists (p);
    }
    else
    {
      // Check the alternative name first since it is more specific.
      //
      p = d / a;

      if ((e = exists (p)))
        altn = true;
      else
      {
        p = d / s;

        if ((e = exists (p)))
          altn = false;
      }
    }

    return e ? p : T ();
  }

  bool
  is_src_root (const dir_path& d, optional<bool>& altn)
  {
    // We can't have root without bootstrap.build.
    //
    return !exists (d, std_bootstrap_file, alt_bootstrap_file, altn).empty ();
  }

  bool
  is_out_root (const dir_path& d, optional<bool>& altn)
  {
    return !exists (d, std_src_root_file, alt_src_root_file, altn).empty ();
  }

  dir_path
  find_src_root (const dir_path& b, optional<bool>& altn)
  {
    assert (b.absolute ());

    for (dir_path d (b); !d.root () && d != home; d = d.directory ())
    {
      if (is_src_root (d, altn))
        return d;
    }

    return dir_path ();
  }

  pair<dir_path, bool>
  find_out_root (const dir_path& b, optional<bool>& altn)
  {
    assert (b.absolute ());

    for (dir_path d (b); !d.root () && d != home; d = d.directory ())
    {
      bool s;
      if ((s = is_src_root (d, altn)) || is_out_root (d, altn))
        return make_pair (move (d), s);
    }

    return make_pair (dir_path (), false);
  }

  optional<path>
  find_buildfile (const dir_path& sd,
                  const dir_path& root,
                  optional<bool>& altn,
                  const path& n)
  {
    if (n.string () == "-")
      return n;

    path f;
    dir_path p;

    for (;;)
    {
      const dir_path& d (p.empty () ? sd : p.directory ());

      // Note that we don't attempt to derive the project's naming scheme
      // from the buildfile name specified by the user.
      //
      bool e;
      if (!n.empty () || altn)
      {
        f = d / (!n.empty () ? n : (*altn
                                    ? alt_buildfile_file
                                    : std_buildfile_file));
        e = exists (f);
      }
      else
      {
        // Note: this case seems to be only needed for simple projects.
        //

        // Check the alternative name first since it is more specific.
        //
        f = d / alt_buildfile_file;

        if ((e = exists (f)))
          altn = true;
        else
        {
          f = d / std_buildfile_file;

          if ((e = exists (f)))
            altn = false;
        }
      }

      if (e)
        return f;

      p = f.directory ();
      if (p == root)
        break;
    }

    return nullopt;
  }

  optional<path>
  find_plausible_buildfile (const name& tgt,
                            const scope& rs,
                            const dir_path& src_base,
                            const dir_path& src_root,
                            optional<bool>& altn,
                            const path& name)
  {
    // If we cannot find the buildfile in this directory, then try our luck
    // with the nearest outer buildfile, in case our target is defined there
    // (common with non-intrusive project conversions where everything is
    // built from a single root buildfile).
    //
    // The directory target case is ambigous since it can also be the implied
    // buildfile. The heuristics that we use is to check whether the implied
    // buildfile is plausible: there is a subdirectory with a buildfile.
    // Checking for plausability feels expensive since we have to recursively
    // traverse the directory tree. Note, however, that if the answer is
    // positive, then shortly after we will be traversing this tree anyway and
    // presumably this time getting the data from the cache (we don't really
    // care about the negative answer since this is a degenerate case).
    //
    optional<path> bf;

    // If the target is a directory and the implied buildfile is plausible,
    // then assume that. Otherwise, search for an outer buildfile.
    //
    if ((tgt.directory () || tgt.type == "dir") &&
        exists (src_base)                       &&
        dir::check_implied (rs, src_base))
      bf = path (); // Leave empty.
    else
    {
      if (src_base != src_root)
        bf = find_buildfile (src_base.directory (), src_root, altn, name);
    }

    return bf;
  }

  // Remap the src_root variable value if it is inside old_src_root.
  //
  static inline void
  remap_src_root (context& ctx, value& v)
  {
    if (!ctx.old_src_root.empty ())
    {
      dir_path& d (cast<dir_path> (v));

      if (d.sub (ctx.old_src_root))
        d = ctx.new_src_root / d.leaf (ctx.old_src_root);
    }
  }

  static void
  source (parser& p, scope& root, scope& base, lexer& l)
  {
    tracer trace ("source");

    const path_name& fn (l.name ());

    try
    {
      l5 ([&]{trace << "sourcing " << fn;});
      p.parse_buildfile (l, &root, base);
    }
    catch (const io_error& e)
    {
      fail << "unable to read buildfile " << fn << ": " << e;
    }
  }

  static inline void
  source (parser& p,
          scope& root,
          scope& base,
          istream& is,
          const path_name& in)
  {
    lexer l (is, in);
    source (p, root, base, l);
  }

  static void
  source (parser& p, scope& root, scope& base, const path& bf)
  {
    path_name fn (bf);
    try
    {
      ifdstream ifs;
      return source (p, root, base, open_file_or_stdin (fn, ifs), fn);
    }
    catch (const io_error& e)
    {
      fail << "unable to read buildfile " << fn << ": " << e;
    }
  }

  static bool
  source_once (parser& p,
               scope& root,
               scope& base,
               const path& bf,
               scope& once)
  {
    tracer trace ("source_once");

    if (!once.root_extra->insert_buildfile (bf))
    {
      l5 ([&]{trace << "skipping already sourced " << bf;});
      return false;
    }

    source (p, root, base, bf);
    return true;
  }

  void
  source (scope& root, scope& base, const path& bf)
  {
    parser p (root.ctx);
    source (p, root, base, bf);
  }

  void
  source (scope& root, scope& base, istream& is, const path_name& in)
  {
    parser p (root.ctx);
    source (p, root, base, is, in);
  }

  void
  source (scope& root, scope& base, lexer& l, load_stage s)
  {
    parser p (root.ctx, s);
    source (p, root, base, l);
  }

  bool
  source_once (scope& root, scope& base, const path& bf, scope& once)
  {
    parser p (root.ctx);
    return source_once (p, root, base, bf, once);
  }

  // Source (once) pre-*.build (pre is true) or post-*.build (otherwise) hooks
  // from the specified directory (build/{bootstrap,root}/ of out_root) which
  // must exist.
  //
  static void
  source_hooks (parser& p, scope& root, const dir_path& d, bool pre)
  {
    // While we could have used the wildcard pattern matching functionality,
    // our needs are pretty basic and performance is quite important, so let's
    // handle this ourselves.
    //
    try
    {
      for (const dir_entry& de: dir_iterator (d, dir_iterator::no_follow))
      {
        // If this is a link, then type() will try to stat() it. And if the
        // link is dangling or points to something inaccessible, it will fail.
        // So let's first check that the name matches and only then check the
        // type.
        //
        const path& n (de.path ());

        if (n.string ().compare (0,
                                 pre ? 4 : 5,
                                 pre ? "pre-" : "post-") != 0 ||
            n.extension () != root.root_extra->build_ext)
          continue;

        path f (d / n);

        try
        {
          if (de.type () != entry_type::regular)
            continue;
        }
        catch (const system_error& e)
        {
          fail << "unable to read buildfile " << f << ": " << e;
        }

        source_once (p, root, root, f, root);
      }
    }
    catch (const system_error& e)
    {
      fail << "unable to iterate over " << d << ": " << e;
    }
  }

  scope_map::iterator
  create_root (context& ctx,
               const dir_path& out_root,
               const dir_path& src_root)
  {
    auto i (ctx.scopes.rw ().insert_out (out_root, true /* root */));
    scope& rs (*i->second.front ());

    // Set out_path. Note that src_path is set in setup_root() below.
    //
    if (rs.out_path_ != &i->first)
    {
      assert (rs.out_path_ == nullptr);
      rs.out_path_ = &i->first;
    }

    // If this is already a root scope, verify that things are consistent.
    //
    {
      value& v (rs.assign (ctx.var_out_root));

      if (!v)
        v = out_root;
      else
      {
        const dir_path& p (cast<dir_path> (v));

        if (p != out_root)
          fail << "new out_root " << out_root << " does not match "
               << "existing " << p;
      }
    }

    if (!src_root.empty ())
    {
      value& v (rs.assign (ctx.var_src_root));

      if (!v)
        v = src_root;
      else
      {
        const dir_path& p (cast<dir_path> (v));

        if (p != src_root)
          fail << "new src_root " << src_root << " does not match "
               << "existing " << p;
      }
    }

    return i;
  }

  void
  setup_root (scope& s, bool forwarded)
  {
    context& ctx (s.ctx);

    // The caller must have made sure src_root is set on this scope.
    //
    value& v (s.assign (ctx.var_src_root));
    assert (v);
    const dir_path& d (cast<dir_path> (v));

    if (s.src_path_ == nullptr)
    {
      if (*s.out_path_ != d)
      {
        auto i (ctx.scopes.rw (s).insert_src (s, d));
        s.src_path_ = &i->first;
      }
      else
        s.src_path_ = s.out_path_;
    }
    else
      assert (*s.src_path_ == d);

    s.assign (ctx.var_forwarded) = forwarded;
  }

  scope&
  setup_base (scope_map::iterator i,
              const dir_path& out_base,
              const dir_path& src_base)
  {
    scope& s (*i->second.front ());
    context& ctx (s.ctx);

    // Set src/out_base variables.
    //
    value& ov (s.assign (ctx.var_out_base));

    if (!ov)
      ov = out_base;
    else
      assert (cast<dir_path> (ov) == out_base);

    value& sv (s.assign (ctx.var_src_base));

    if (!sv)
      sv = src_base;
    else
      assert (cast<dir_path> (sv) == src_base);

    // Set src/out_path. The key (i->first) is out_base.
    //
    if (s.out_path_ == nullptr)
      s.out_path_ = &i->first;
    else
      assert (*s.out_path_ == out_base);

    if (s.src_path_ == nullptr)
    {
      if (out_base != src_base)
      {
        auto i (ctx.scopes.rw (s).insert_src (s, src_base));
        s.src_path_ = &i->first;
      }
      else
        s.src_path_ = s.out_path_;
    }
    else
      assert (*s.src_path_ == src_base);

    return s;
  }

  pair<scope&, scope*>
  switch_scope (scope& root, const dir_path& out_base, bool proj)
  {
    context& ctx (root.ctx);

    assert (ctx.phase == run_phase::load);

    // First, enter the scope into the map and see if it is in any project. If
    // it is not, then there is nothing else to do.
    //
    auto i (ctx.scopes.rw (root).insert_out (out_base));
    scope& base (*i->second.front ());

    scope* rs (nullptr);

    if (proj && (rs = base.root_scope ()) != nullptr)
    {
      // The path must be in the out (since we've inserted it as out into the
      // scope map).
      //
      assert (out_base.sub (rs->out_path ()));

      // Create and bootstrap root scope(s) of subproject(s) that this scope
      // may belong to. If any were created, load them. Note that we need to
      // do this before figuring out src_base since we may switch the root
      // project (and src_root with it).
      //
      rs = &create_bootstrap_inner (*rs, out_base);

      // Switch to the new root scope.
      //
      if (rs != &root && !rs->root_extra->loaded)
        load_root (*rs); // Load new root(s) recursively.

      // Now we can figure out src_base and finish setting the scope.
      //
      setup_base (i, out_base, src_out (out_base, *rs));
    }

    return pair<scope&, scope*> (base, rs);
  }

  dir_path
  bootstrap_fwd (context& ctx, const dir_path& src_root, optional<bool>& altn)
  {
    path f (exists (src_root, std_out_root_file, alt_out_root_file, altn));

    if (f.empty ())
      return src_root;

    // We cannot just source the buildfile since there is no scope to do
    // this on yet.
    //
    if (optional<value> v = extract_variable (ctx, f, *ctx.var_out_root))
    {
      auto r (convert<dir_path> (move (*v)));

      if (r.relative ())
        fail << "relative path in out_root value in " << f;

      return r;
    }
    else
      fail << "variable out_root expected as first line in " << f << endf;
  }

  scope::root_extra_type::
  root_extra_type (scope& root, bool a)
      : altn (a),
        loaded (false),

        build_ext        (a ? alt_build_ext        : std_build_ext),
        build_dir        (a ? alt_build_dir        : std_build_dir),
        buildfile_file   (a ? alt_buildfile_file   : std_buildfile_file),
        buildignore_file (a ? alt_buildignore_file : std_buildignore_file),
        root_dir         (a ? alt_root_dir         : std_root_dir),
        bootstrap_dir    (a ? alt_bootstrap_dir    : std_bootstrap_dir),
        build_build_dir  (a ? alt_build_build_dir  : std_build_build_dir),
        bootstrap_file   (a ? alt_bootstrap_file   : std_bootstrap_file),
        root_file        (a ? alt_root_file        : std_root_file),
        export_file      (a ? alt_export_file      : std_export_file),
        src_root_file    (a ? alt_src_root_file    : std_src_root_file),
        out_root_file    (a ? alt_out_root_file    : std_out_root_file),

        var_pool (&root.ctx, &root.ctx.var_pool.rw (root), nullptr)
  {
    root.var_pool_ = &var_pool;
  }

  static void
  setup_root_extra (scope& root, optional<bool>& altn)
  {
    assert (altn && root.root_extra == nullptr);

    context& ctx (root.ctx);

    root.root_extra.reset (new scope::root_extra_type (root, *altn));

    // Enter built-in meta-operation and operation names. Loading of
    // modules (via the src bootstrap; see below) can result in
    // additional meta/operations being added.
    //
    root.insert_meta_operation (noop_id,    mo_noop);
    root.insert_meta_operation (perform_id, mo_perform);
    root.insert_meta_operation (info_id,    mo_info);

    root.insert_operation (default_id, op_default, nullptr);
    root.insert_operation (update_id,  op_update,  ctx.var_update);
    root.insert_operation (clean_id,   op_clean,   ctx.var_clean);
  }

  value&
  bootstrap_out (scope& root, optional<bool>& altn)
  {
    context& ctx (root.ctx);
    const dir_path& out_root (root.out_path ());

    path f (exists (out_root, std_src_root_file, alt_src_root_file, altn));

    if (!f.empty ())
    {
      if (root.root_extra == nullptr)
        setup_root_extra (root, altn);

      //@@ TODO: if bootstrap files can source other bootstrap files (for
      //   example, as a way to express dependecies), then we need a way to
      //   prevent multiple sourcing. We handle it here but we still need
      //   something like source_once (once [scope] source) in buildfiles.
      //
      parser p (ctx, load_stage::boot);
      source_once (p, root, root, f, root);
    }

    value& v (root.assign (ctx.var_src_root));

    if (!f.empty ())
    {
      // Verify the value set by src-root.build is sensible.
      //
      // Note: keeping diagnostics consistent with bootstrap_fwd() and
      // find_project_name().
      //
      if (!v)
        fail << "variable src_root expected as first line in " << f;

      if (cast<dir_path> (v).relative ())
        fail << "relative path in src_root value in " << f;
    }

    return v;
  }

  optional<value>
  extract_variable (context& ctx, lexer& l, const variable& var)
  {
    const path_name& fn (l.name ());

    try
    {
      token t (l.next ());

      token_type tt;
      if (t.type != token_type::word || t.value != var.name ||
          ((tt = l.next ().type) != token_type::assign &&
           tt != token_type::prepend &&
           tt != token_type::append))
      {
        return nullopt;
      }

      parser p (ctx);
      temp_scope tmp (ctx.global_scope.rw ());
      p.parse_variable (l, tmp, var, tt);

      value* v (tmp.vars.lookup_to_modify (var).first);
      assert (v != nullptr);

      // Steal the value, the scope is going away.
      //
      return move (*v);
    }
    catch (const io_error& e)
    {
      fail << "unable to read buildfile " << fn << ": " << e << endf;
    }
  }

  optional<value>
  extract_variable (context& ctx,
                    istream& is, const path& bf,
                    const variable& var)
  {
    path_name in (bf);
    lexer l (is, in);
    return extract_variable (ctx, l, var);
  }

  optional<value>
  extract_variable (context& ctx, const path& bf, const variable& var)
  {
    try
    {
      ifdstream ifs (bf);
      return extract_variable (ctx, ifs, bf, var);
    }
    catch (const io_error& e)
    {
      fail << "unable to read buildfile " << bf << ": " << e << endf;
    }
  }

  // Extract the project name from bootstrap.build.
  //
  static project_name
  find_project_name (context& ctx,
                     const dir_path& out_root,
                     const dir_path& fallback_src_root,
                     optional<bool> out_src, // True if out_root is src_root.
                     optional<bool>& altn)
  {
    tracer trace ("find_project_name");

    // First check if the root scope for this project has already been setup
    // in which case we will have src_root and maybe even the name.
    //
    const dir_path* src_root (nullptr);
    const scope& s (ctx.scopes.find_out (out_root));

    if (s.root_scope () == &s && s.out_path () == out_root)
    {
      if (s.root_extra != nullptr)
      {
        if (!altn)
          altn = s.root_extra->altn;
        else
          assert (*altn == s.root_extra->altn);

        if (s.root_extra->project)
        {
          return (*s.root_extra->project != nullptr
                  ? **s.root_extra->project
                  : empty_project_name);
        }
      }

      src_root = s.src_path_;
    }

    // Load the project name. If this subdirectory is the subproject's
    // src_root, then we can get directly to that. Otherwise, we first have to
    // discover its src_root.
    //
    value src_root_v; // Need it to live until the end.

    if (src_root == nullptr)
    {
      if (out_src ? *out_src : is_src_root (out_root, altn))
        src_root = &out_root;
      else
      {
        path f (exists (out_root, std_src_root_file, alt_src_root_file, altn));

        if (f.empty ())
        {
          // Note: the same diagnostics as in main().
          //
          if (fallback_src_root.empty ())
            fail << "no bootstrapped src_root for " << out_root <<
              info << "consider reconfiguring this out_root";

          src_root = &fallback_src_root;
        }
        else
        {
          optional<value> v (extract_variable (ctx, f, *ctx.var_src_root));

          if (!v)
            fail << "variable src_root expected as first line in " << f;

          if (cast<dir_path> (*v).relative ())
            fail << "relative path in src_root value in " << f;

          src_root_v = move (*v);
          remap_src_root (ctx, src_root_v); // Remap if inside old_src_root.
          src_root = &cast<dir_path> (src_root_v);

          l5 ([&]{trace << "extracted src_root " << *src_root
                        << " for " << out_root;});
        }
      }
    }

    project_name name;
    {
      path f (exists (*src_root, std_bootstrap_file, alt_bootstrap_file, altn));

      if (f.empty ())
        fail << "no build/bootstrap.build in " << *src_root;

      if (optional<value> v = extract_variable (ctx, f, *ctx.var_project))
      {
        name = cast<project_name> (move (*v));
      }
      else
        fail << "variable " << *ctx.var_project << " expected as a first "
             << "line in " << f;
    }

    l5 ([&]{trace << "extracted project name '" << name << "' for "
                  << *src_root;});
    return name;
  }

  // Scan the specified directory for any subprojects. If a subdirectory
  // is a subproject, then enter it into the map, handling the duplicates.
  //
  static void
  find_subprojects (context& ctx,
                    subprojects& sps,
                    const dir_path& d,
                    const dir_path& root,
                    bool out)
  {
    tracer trace ("find_subprojects");

    try
    {
      // It's probably possible that a subproject can be a symlink with the
      // link target, for example, being in a git submodule. Considering that,
      // it makes sense to warn about dangling symlinks.
      //
      for (const dir_entry& de:
             dir_iterator (d, dir_iterator::detect_dangling))
      {
        const path& n (de.path ());

        // Skip hidden entries.
        //
        if (n.empty () || n.string ().front () == '.')
          continue;

        if (de.type () != entry_type::directory)
        {
          if (de.type () == entry_type::unknown)
          {
            bool sl (de.ltype () == entry_type::symlink);

            warn << "skipping "
                 << (sl ? "dangling symlink" : "inaccessible entry") << ' '
                 << d / n;
          }

          continue;
        }

        dir_path sd (d / path_cast<dir_path> (n));

        bool src (false);
        optional<bool> altn;

        if (!((out && is_out_root (sd, altn)) ||
              (src =  is_src_root (sd, altn))))
        {
          // We used to scan for subproject recursively but this is probably
          // too loose (think of some tests laying around). In the future we
          // should probably allow specifying something like extra/* or
          // extra/** in subprojects.
          //
          //find_subprojects (sps, sd, root, out);
          //
          continue;
        }

        // Calculate relative subdirectory for this subproject.
        //
        dir_path dir (sd.leaf (root));
        l5 ([&]{trace << "subproject " << sd << " as " << dir;});

        // Load its name. Note that here we don't use fallback src_root
        // since this function is used to scan both out_root and src_root.
        //
        project_name name (
          find_project_name (ctx, sd, dir_path (), src, altn));

        // If the name is empty, then is is an unnamed project. While the
        // 'project' variable stays empty, here we come up with a surrogate
        // name for a key. The idea is that such a key should never conflict
        // with a real project name. We ensure this by using the project's
        // sub-directory and appending a trailing directory separator to it.
        //
        if (name.empty ())
          name = project_name (dir.posix_string () + '/',
                               project_name::raw_string);

        // @@ Can't use move() because we may need the values in diagnostics
        // below. Looks like C++17 try_emplace() is what we need.
        //
        auto rp (sps.emplace (name, dir));

        // Handle duplicates.
        //
        if (!rp.second)
        {
          const dir_path& dir1 (rp.first->second);

          if (dir != dir1)
            fail << "inconsistent subproject directories for " << name <<
              info << "first alternative: " << dir1 <<
              info << "second alternative: " << dir;

          l6 ([&]{trace << "skipping duplicate";});
        }
      }
    }
    catch (const system_error& e)
    {
      fail << "unable to iterate over " << d << ": " << e;
    }
  }

  void
  bootstrap_src (scope& rs, optional<bool>& altn,
                 optional<dir_path> aovr,
                 bool sovr)
  {
    tracer trace ("bootstrap_src");

    context& ctx (rs.ctx);

    const dir_path& out_root (rs.out_path ());
    const dir_path& src_root (rs.src_path ());

    path bf (exists (src_root, std_bootstrap_file, alt_bootstrap_file, altn));

    if (rs.root_extra == nullptr)
    {
      // If nothing so far has indicated the naming, assume standard.
      //
      if (!altn)
        altn = false;

      setup_root_extra (rs, altn);
    }

    bool simple (bf.empty ());

    if (simple)
    {
      // Simple project: no name, disabled amalgamation, no subprojects.
      //
      rs.root_extra->project = nullptr;
      rs.root_extra->amalgamation = nullptr;
      rs.root_extra->subprojects = nullptr;

      // See GH issue #322.
      //
#if 0
      assert (!aovr || aovr->empty ());
#else
      if (!(!aovr || aovr->empty ()))
        fail << "amalgamation directory " << *aovr << " specified for simple "
             << "project " << src_root <<
          info << "see https://github.com/build2/build2/issues/322 for details";
#endif
    }
    // We assume that bootstrap out cannot load this file explicitly. It
    // feels wrong to allow this since that makes the whole bootstrap
    // process hard to reason about. But we may try to bootstrap the same
    // root scope multiple time.
    //
    else if (rs.root_extra->insert_buildfile (bf))
    {
      // Extract the project name and amalgamation variable value so that
      // we can make them available while loading bootstrap.build.
      //
      // In case of amalgamation, we only deal with the empty variable value
      // (which indicates that amalgamating this project is disabled). We go
      // through all this trouble of extracting its value manually (and thus
      // requiring its assignment, if any, to be the second line in
      // bootstrap.build, after project assignment) in order to have the
      // logical amalgamation view during bootstrap (note that the bootstrap
      // pre hooks will still see physical amalgamation).
      //
      optional<value> pv, av;
      try
      {
        ifdstream ifs (bf);
        path_name bfn (bf);
        lexer l (ifs, bfn);

        pv = extract_variable (ctx, l, *ctx.var_project);

        if (!pv)
          fail << "variable " << *ctx.var_project << " expected as a first "
               << "line in " << bf;

        av = extract_variable (ctx, l, *ctx.var_amalgamation);
      }
      catch (const io_error& e)
      {
        fail << "unable to read buildfile " << bf << ": " << e;
      }

      const project_name pn (cast<project_name> (move (*pv)));
      rs.root_extra->project = &pn;

      // @@ We will still have original values in the variables during
      //    bootstrap. Not sure what we can do about that. But it seems
      //    harmless.
      //
      if (aovr)
        rs.root_extra->amalgamation = aovr->empty () ? nullptr : &*aovr;
      else if (av && (av->null || av->empty ()))
        rs.root_extra->amalgamation = nullptr;

      {
        parser p (rs.ctx, load_stage::boot);
        source (p, rs, rs, bf);
      }

      // Update to point to the variable value.
      //
      rs.root_extra->project = &cast<project_name> (rs.vars[ctx.var_project]);

      // Detect and diagnose the case where the amalgamation variable is not
      // the second line.
      //
      if (!av && rs.vars[ctx.var_amalgamation].defined ())
      {
        fail << "variable " << *ctx.var_amalgamation << " expected as a "
             << "second line in " << bf;
      }

      // Replace the value if overridden.
      //
      // Note that root_extra::amalgamation will be re-pointed below.
      //
      if (aovr)
        rs.vars.assign (ctx.var_amalgamation) = move (*aovr);
    }
    else
    {
      // Here we assume amalgamation has been dealt with.
      //
      l5 ([&]{trace << "skipping already sourced " << bf;});
    }

    // Finish dealing with the amalgamation. There are two key players: the
    // outer root scope which may already be present (i.e., we were loaded as
    // part of an amalgamation) and the amalgamation variable that may or may
    // not be set by the user (in bootstrap.build) or by an earlier call to
    // this function for the same scope. When set by the user, the empty
    // special value means that the project shall not be amalgamated (and
    // which we convert to NULL below). When calculated, the NULL value
    // indicates that we are not amalgamated.
    //
    // Before we used to assume that if there is an outer root scope, then
    // that got to be our amalgamation. But it turns our this is not always
    // the case (for example, a private host configuration in bpkg) and there
    // could be an unbootstrapped project between us and an outer root scope.
    //
    // Note: the amalgamation variable value is always a relative directory.
    //
    if (!simple)
    {
      auto rp (rs.vars.insert (*ctx.var_amalgamation)); // Set NULL by default.
      value& v (rp.first);

      if (v && v.empty ()) // Convert empty to NULL.
        v = nullptr;

      scope* ars (rs.parent_scope ()->root_scope ());

      if (rp.second)
      {
        // If the amalgamation variable hasn't been set, then we need to check
        // if any of the outer directories is a project's out_root. If so,
        // then that's (likely) our amalgamation.
        //
        optional<bool> altn;
        const dir_path& d (find_out_root (out_root.directory (), altn).first);

        if (!d.empty ())
        {
          // Note that the sub() test is important: during configuration we
          // may find a project that is outside the outer root scope in which
          // case we should use the latter instead.
          //
          if (ars == nullptr ||
              (d != ars->out_path () && d.sub (ars->out_path ())))
          {
            dir_path rd (d.relative (out_root));
            l5 ([&]{trace << out_root << " amalgamated as " << rd;});
            v = move (rd);
            ars = nullptr; // Skip the checks blow.
          }
          // Else fall through.
        }
        else
        {
          // Note that here ars may be not NULL. This can happen both when ars
          // is a simple project or if out_root is in out directory that has
          // no been configured. In this case falling through is what we want.
        }
      }
      else if (v)
      {
        if (cast<dir_path> (v).absolute ())
          fail << "absolute directory in variable " << *ctx.var_amalgamation
               << " value";
      }

      // Do additional checks if the outer root could be our amalgamation.
      //
      if (ars != nullptr)
      {
        const dir_path& ad (ars->out_path ());

        // If we have the amalgamation variable set by the user, verify that
        // it's a subdirectory of the outer root scope.
        //
        // Note that in this case we allow amalgamation by a simple project
        // (we rely on this, for example, in our modules sidebuild machinery).
        //
        if (!rp.second)
        {
          if (v)
          {
            const dir_path& vd (cast<dir_path> (v));
            dir_path d (out_root / vd);
            d.normalize ();

            if (!d.sub (ad))
              fail << "incorrect amalgamation " << vd << " of " << out_root;
          }
        }
        // By default we do not get amalgamated by a simple project.
        //
        else if (!(ars->root_extra->project &&
                   *ars->root_extra->project == nullptr))
        {
          // Otherwise, use the outer root as our amalgamation.
          //
          dir_path rd (ad.relative (out_root));

          l5 ([&]{trace << out_root << " amalgamated as " << rd;});
          v = move (rd);
        }
      }

      rs.root_extra->amalgamation = cast_null<dir_path> (v);
    }

    // See if we have any subprojects. In a sense, this is the other
    // side/direction of the amalgamation logic above. Here, the subprojects
    // variable may or may not be set by the user (in bootstrap.build) or by
    // an earlier call to this function for the same scope. When set by the
    // user, the empty special value means that there are no subproject and
    // none should be searched for (and which we convert to NULL below).
    // Otherwise, it is a list of [project@]directory pairs. The directory
    // must be relative to our out_root. If the project name is not specified,
    // then we have to figure it out. When subprojects are calculated, the
    // NULL value indicates that we found no subprojects.
    //
    if (!simple)
    {
      auto rp (rs.vars.insert (*ctx.var_subprojects)); // Set NULL by default.
      value& v (rp.first);

      if (!sovr)
      {
        if (rp.second)
          rp.second = false; // Keep NULL.
        else
          v = nullptr; // Make NULL.
      }

      if (rp.second)
      {
        // No subprojects set so we need to figure out if there are any.
        //
        // First we are going to scan our out_root and find all the
        // pre-configured subprojects. Then, if out_root != src_root,
        // we are going to do the same for src_root. Here, however,
        // we need to watch out for duplicates.
        //
        subprojects sps;

        if (exists (out_root))
        {
          l5 ([&]{trace << "looking for subprojects in " << out_root;});
          find_subprojects (rs.ctx, sps, out_root, out_root, true);
        }

        if (out_root != src_root)
        {
          l5 ([&]{trace << "looking for subprojects in " << src_root;});
          find_subprojects (rs.ctx, sps, src_root, src_root, false);
        }

        if (!sps.empty ()) // Keep it NULL if no subprojects.
          v = move (sps);
      }
      else if (v)
      {
        // Convert empty to NULL.
        //
        if (v.empty ())
          v = nullptr;
        else
        {
          // Scan the (untyped) value and convert it to the "canonical" form,
          // that is, a list of name@dir pairs.
          //
          subprojects sps;
          names& ns (cast<names> (v));

          for (auto i (ns.begin ()); i != ns.end (); ++i)
          {
            // Project name.
            //
            project_name n;
            if (i->pair)
            {
              if (i->pair != '@')
                fail << "unexpected pair style in variable subprojects";

              try
              {
                n = convert<project_name> (move (*i));

                if (n.empty ())
                  fail << "empty project name in variable subprojects";
              }
              catch (const invalid_argument&)
              {
                fail << "expected project name instead of '" << *i << "' in "
                     << "variable subprojects";
              }

              ++i; // Got to have the second half of the pair.
            }

            // Directory.
            //
            dir_path d;
            try
            {
              d = convert<dir_path> (move (*i));

              if (d.empty ())
                fail << "empty directory in variable subprojects";
            }
            catch (const invalid_argument&)
            {
              fail << "expected directory instead of '" << *i << "' in "
                   << "variable subprojects";
            }

            // Figure out the project name if the user didn't specify one.
            //
            if (n.empty ())
            {
              optional<bool> altn;

              // Pass fallback src_root since this is a subproject that was
              // specified by the user so it is most likely in our src.
              //
              n = find_project_name (rs.ctx,
                                     out_root / d,
                                     src_root / d,
                                     nullopt /* out_src */,
                                     altn);

              // See find_subprojects() for details on unnamed projects.
              //
              if (n.empty ())
                n = project_name (d.posix_string () + '/',
                                  project_name::raw_string);
            }

            sps.emplace (move (n), move (d));
          }

          // Change the value to the typed map.
          //
          v = move (sps);
        }
      }

      rs.root_extra->subprojects = cast_null<subprojects> (v);
    }
  }

  void
  bootstrap_pre (scope& root, optional<bool>& altn)
  {
    const dir_path& out_root (root.out_path ());

    // This test is a bit loose in a sense that there can be a stray
    // build/bootstrap/ directory that will make us mis-treat a project as
    // following the standard naming scheme (the other way, while also
    // possible, is a lot less likely). If this does becomes a problem, we can
    // always tighten the test by also looking for a hook file with the
    // correct extension.
    //
    dir_path d (exists (out_root, std_bootstrap_dir, alt_bootstrap_dir, altn));

    if (!d.empty ())
    {
      if (root.root_extra == nullptr)
        setup_root_extra (root, altn);

      parser p (root.ctx, load_stage::boot);
      source_hooks (p, root, d, true /* pre */);
    }
  }

  void
  bootstrap_post (scope& root)
  {
    const dir_path& out_root (root.out_path ());

    dir_path d (out_root / root.root_extra->bootstrap_dir);

    if (exists (d))
    {
      parser p (root.ctx, load_stage::boot);
      source_hooks (p, root, d, false /* pre */);
    }

    // Call module's post-boot functions.
    //
    for (size_t i (0); i != root.root_extra->loaded_modules.size (); ++i)
    {
      module_state& s (root.root_extra->loaded_modules[i]);

      if (s.boot_post != nullptr)
        boot_post_module (root, s);
    }
  }

  bool
  bootstrapped (scope& rs)
  {
    // Use the subprojects value cached at the end of bootstrap_src() as an
    // indicator.
    //
    return rs.root_extra != nullptr && rs.root_extra->subprojects;
  }

  // Return true if the inner/outer project (identified by out/src_root) of
  // the 'origin' project (identified by orig) should be forwarded.
  //
  static inline bool
  forwarded (const scope& orig,
             const dir_path& out_root,
             const dir_path& src_root,
             optional<bool>& altn)
  {
    context& ctx (orig.ctx);

    // The conditions are:
    //
    // 1. Origin is itself forwarded.
    //
    // 2. Inner/outer src_root != out_root.
    //
    // 3. Inner/outer out-root.build exists in src_root and refers out_root.
    //
    return (out_root != src_root                            &&
            cast_false<bool> (orig.vars[ctx.var_forwarded]) &&
            bootstrap_fwd (ctx, src_root, altn) == out_root);
  }

  void
  create_bootstrap_outer (scope& root, bool subp)
  {
    context& ctx (root.ctx);

    auto l (root.vars[ctx.var_amalgamation]);

    if (!l)
      return;

    const dir_path& d (cast<dir_path> (l));
    dir_path out_root (root.out_path () / d);
    out_root.normalize (); // No need to actualize (d is a bunch of ..)

    // src_root is a bit more complicated. Here we have three cases:
    //
    // 1. Amalgamation's src_root is "parallel" to the sub-project's.
    // 2. Amalgamation's src_root is the same as its out_root.
    // 3. Some other pre-configured (via src-root.build) src_root.
    //
    // So we need to try all these cases in some sensible order. #3 should
    // probably be tried first since that src_root was explicitly configured
    // by the user. After that, #2 followed by #1 seems reasonable.
    //
    scope& rs (*create_root (ctx, out_root, dir_path ())->second.front ());

    bool bstrapped (bootstrapped (rs));

    optional<bool> altn;
    if (!bstrapped)
    {
      value& v (bootstrap_out (rs, altn)); // #3 happens here (or it can be #1)

      if (!v)
      {
        if (is_src_root (out_root, altn)) // #2
          v = out_root;
        else // #1
        {
          dir_path src_root (root.src_path () / d);
          src_root.normalize (); // No need to actualize (as above).
          v = move (src_root);
        }
      }
      else
        remap_src_root (ctx, v); // Remap if inside old_src_root.

      setup_root (rs, forwarded (root, out_root, v.as<dir_path> (), altn));
      bootstrap_pre (rs, altn);
      bootstrap_src (rs, altn, nullopt, subp);
      // bootstrap_post() delayed until after create_bootstrap_outer().
    }
    else
    {
      altn = rs.root_extra->altn;

      if (forwarded (root, rs.out_path (), rs.src_path (), altn))
        rs.assign (ctx.var_forwarded) = true; // Only upgrade (see main()).
    }

    create_bootstrap_outer (rs, subp);

    if (!bstrapped)
      bootstrap_post (rs);

    // Check if we are strongly amalgamated by this outer root scope.
    //
    // Note that we won't end up here if we are not amalgamatable.
    //
    if (root.src_path ().sub (rs.src_path ()))
      root.strong_ = rs.strong_scope (); // Itself or some outer scope.
  }

  scope&
  create_bootstrap_inner (scope& root, const dir_path& out_base)
  {
    context& ctx (root.ctx);

    scope* r (&root);

    if (const subprojects* ps = *root.root_extra->subprojects)
    {
      for (const auto& p: *ps)
      {
        dir_path out_root (root.out_path () / p.second);

        if (!out_base.empty () && !out_base.sub (out_root))
          continue;

        // The same logic to src_root as in create_bootstrap_outer().
        //
        scope& rs (*create_root (ctx, out_root, dir_path ())->second.front ());

        optional<bool> altn;
        if (!bootstrapped (rs))
        {
          // Clear current project's environment.
          //
          auto_project_env penv (nullptr);

          value& v (bootstrap_out (rs, altn));

          if (!v)
          {
            v = is_src_root (out_root, altn)
              ? out_root
              : (root.src_path () / p.second);
          }
          else
            remap_src_root (ctx, v); // Remap if inside old_src_root.

          setup_root (rs, forwarded (root, out_root, v.as<dir_path> (), altn));
          bootstrap_pre (rs, altn);
          bootstrap_src (rs, altn);
          bootstrap_post (rs);
        }
        else
        {
          altn = rs.root_extra->altn;
          if (forwarded (root, rs.out_path (), rs.src_path (), altn))
            rs.assign (ctx.var_forwarded) = true; // Only upgrade (see main()).
        }

        //@@ TODO: what if subproject has amalgamation disabled? Can we have a
        //         subproject that disables our attempt to amalgamate it (see
        //         amalgamatable() call below).

        // Check if we strongly amalgamated this inner root scope.
        //
        if (rs.amalgamatable ())
        {
          if (rs.src_path ().sub (root.src_path ()))
            rs.strong_ = root.strong_scope (); // Itself or some outer scope.
        }

        // See if there are more inner roots.
        //
        r = &create_bootstrap_inner (rs, out_base);

        if (!out_base.empty ())
          break; // We have found our subproject.
      }
    }

    return *r;
  }

  void
  load_root (scope& root,
             const function<void (parser&)>& pre,
             const function<void (parser&)>& post)
  {
    tracer trace ("load_root");

    if (root.root_extra->loaded)
    {
      assert (pre == nullptr && post == nullptr);
      return;
    }

    context& ctx (root.ctx);

    if (ctx.no_external_modules)
      fail << "attempt to load project " << root << " after skipped loading "
           << "external modules";

    // First load outer roots, if any.
    //
    if (scope* rs = root.parent_scope ()->root_scope ())
      if (!rs->root_extra->loaded)
        load_root (*rs);

    // Finish off initializing bootstrapped modules (before mode).
    //
    // Note that init() can load additional modules invalidating iterators.
    //
    auto init_modules =
      [&root, n = root.root_extra->loaded_modules.size ()] (module_boot_init v)
    {
      for (size_t i (0); i != n; ++i)
      {
        module_state& s (root.root_extra->loaded_modules[i]);

        if (s.boot_init && *s.boot_init == v)
          init_module (root, root, s.name, s.loc);
      }
    };

    {
      init_modules (module_boot_init::before_first);

      // Project environment should now be in effect.
      //
      auto_project_env penv (root);

      init_modules (module_boot_init::before_second);
      init_modules (module_boot_init::before);
    }

    // Load hooks and root.build.
    //
    const dir_path& out_root (root.out_path ());
    const dir_path& src_root (root.src_path ());

    path f (src_root / root.root_extra->root_file);

    // We can load the pre hooks before finishing off loading the bootstrapped
    // modules (which, in case of config would load config.build) or after and
    // one can come up with a plausible use-case for either approach. Note,
    // however, that one can probably achieve adequate pre-modules behavior
    // with a post-bootstrap hook.
    //
    dir_path hd (out_root / root.root_extra->root_dir);

    bool he (exists (hd));
    bool fe (exists (f));

    // Reuse the parser to accumulate the configuration variable information.
    //
    parser p (ctx, load_stage::root);

    if (pre != nullptr)
    {
      pre (p);
      p.reset ();
    }

    if (he) {source_hooks (p, root, hd, true  /* pre */); p.reset ();}
    if (fe) {source_once (p, root, root, f, root);}
    if (he) {p.reset (); source_hooks (p, root, hd, false /* pre */);}

    if (post != nullptr)
    {
      p.reset ();
      post (p);
    }

    // Finish off initializing bootstrapped modules (after mode).
    //
    {
      auto_project_env penv (root);
      init_modules (module_boot_init::after);
    }

    // Print the project configuration report(s), similar to how we do it in
    // build system modules.
    //
    using config_report = parser::config_report;

    const project_name* proj (nullptr); // Resolve lazily.
    for (const config_report& cr: p.config_reports)
    {
      if (verb < (cr.new_value ? 2 : 3))
        continue;

      if (proj == nullptr)
        proj = &named_project (root); // Can be empty.

      // @@ TODO/MAYBE:
      //
      // - Should we be printing NULL values? Maybe make this configurable?
      // - Quoted printing format (single/double)?

      // Printing the whole variable name would add too much noise with all
      // the repetitive config.<project>. So we are only going to print the
      // part after <project> (see parser::parse_config() for details).
      //
      // But if there is no named project, then we print everything after
      // config. This feels right since there could be zero identifiable
      // information about the project in the header line. For example:
      //
      // config @/tmp/tests
      //   libhello.tests.remote true
      //
      // If the module name is not empty then it means the config variables
      // are from the imported project and so we use that for <project>.
      //
      string stem (!cr.module.empty ()
                   ? '.' + cr.module.variable () + '.'
                   : (!proj->empty ()
                      ? '.' + proj->variable () + '.'
                      : string ()));

      // Return the variable name for printing.
      //
      auto name = [&stem] (const config_report::value& cv) -> const char*
      {
        lookup l (cv.val);

        if (l.value == nullptr)
        {
          if (cv.org.empty ())
            return l.var->name.c_str ();

          // This case may or may not have the prefix.
          //
          size_t p, n (
            !stem.empty ()
            ? (p = cv.org.find (stem)) != string::npos ? p + stem.size () : 0
            : cv.org.compare (0, 7, "config.") == 0 ? 7 : 0);

          return cv.org.c_str () + n;
        }
        else
        {
          assert (cv.org.empty ()); // Sanity check.

          size_t p (!stem.empty ()
                    ? l.var->name.find (stem) + stem.size ()
                    : 7); // "config."

          return l.var->name.c_str () + p;
        }
      };


      // Calculate max name length.
      //
      size_t pad (10);
      for (const config_report::value& cv: cr.values)
      {
        size_t n (strlen (name (cv)));
        if (n > pad)
          pad = n;
      }

      // Use the special `config` module name (which doesn't have its own
      // report) for project's own configuration.
      //
      diag_record dr (text);
      dr << (cr.module.empty () ? "config" : cr.module.string ().c_str ())
         << ' ' << *proj << '@' << root;

      names storage;
      for (const config_report::value& cv: cr.values)
      {
        lookup l (cv.val);
        const string& f (cv.fmt);

        // If the report variable has been overriden, now is the time to
        // lookup its value. Note: see also the name() lambda above if
        // changing anything here.
        //
        string n;
        if (l.value == nullptr)
        {
          n = l.var->name; // Use the name as is.
          l = root[*l.var];
        }
        else
        {
          size_t p (!stem.empty ()
                    ? l.var->name.find (stem) + stem.size ()
                    : 7); // "config."
          n = string (l.var->name, p);
        }

        const char* pn (name (cv)); // Print name.

        dr << "\n  ";

        if (l)
        {
          storage.clear ();
          auto ns (reverse (*l, storage, true /* reduce */));

          if (f == "multiline")
          {
            dr << pn;
            for (auto& n: ns)
              dr << "\n    " << n;
          }
          else
            dr << left << setw (static_cast<int> (pad)) << pn << ' ' << ns;
        }
        else
          dr << left << setw (static_cast<int> (pad)) << pn << " [null]";
      }
    }

    root.root_extra->loaded = true;
  }

  scope&
  load_project (context& ctx,
                const dir_path& out_root,
                const dir_path& src_root,
                bool forwarded,
                bool load)
  {
    assert (ctx.phase == run_phase::load);
    assert (!forwarded || out_root != src_root);

    auto i (create_root (ctx, out_root, src_root));
    scope& rs (*i->second.front ());

    if (!bootstrapped (rs))
    {
      // Clear current project's environment.
      //
      auto_project_env penv (nullptr);

      optional<bool> altn;
      bootstrap_out (rs, altn);
      setup_root (rs, forwarded);
      bootstrap_pre (rs, altn);
      bootstrap_src (rs, altn);
      bootstrap_post (rs);
    }
    else
    {
      if (forwarded)
        rs.assign (ctx.var_forwarded) = true; // Only upgrade (see main()).
    }

    if (load)
    {
      if (!rs.root_extra->loaded)
        load_root (rs);
      setup_base (i, out_root, src_root); // Setup as base.
    }

    return rs;
  }

  // Find or insert a target based on the file path.
  //
  static const target*
  find_target (tracer& trace, context& ctx,
               const target_type& tt, const path& p)
  {
    const target* t (
      ctx.targets.find (tt,
                        p.directory (),
                        dir_path (),
                        p.leaf ().base ().string (),
                        p.extension (),
                        trace));

    if (t != nullptr)
    {
      if (const file* f = t->is_a<file> ())
      {
        // Note that this can happen if we import the same target via two
        // different ways (e.g., installed and via an export stub).
        //
        assert (f->path () == p);
      }
    }

    return t;
  }

  static pair<target&, ulock>
  insert_target (tracer& trace, context& ctx,
                 const target_type& tt, path p)
  {
    auto r (
      ctx.targets.insert_locked (tt,
                                 p.directory (),
                                 dir_path (),    // No out (not in project).
                                 p.leaf ().base ().string (),
                                 p.extension (), // Always specified.
                                 target_decl::implied,
                                 trace));

    if (const file* f = r.first.is_a<file> ())
      f->path (move (p));

    return r;
  }

  // Extract metadata for an executable target by executing it with the
  // --build2-metadata option. Key is the target name (and not necessarily the
  // same as metadata variable prefix in export.metadata; e.g., openbsd-m4 and
  // openbsd_m4). In case of an error, issue diagnostics and fail if opt is
  // false and return nullopt if it's true.
  //
  // Note that loading of the metadata is split into two steps, extraction and
  // parsing, because extraction also serves as validation that the executable
  // is runnable, what we expected, etc. In other words, we sometimes do the
  // extraction without parsing. Actually, this seems to be no longer true but
  // we do separate the two acts with some interleaving code (e.g., inserting
  // the target).
  //
  // Also note that we do not check the export.metadata here leaving it to
  // the caller to do for both this case and export stub.
  //
  // Finally, at first it may seem that caching the metadata is unnecessary
  // since the target state itself serves as a cache (i.e., we try hard to
  // avoid re-extracting the metadata). However, if there is no metadata, then
  // we will re-run the extraction for every optional import. So we cache that
  // case only. Note also that while this is only done during serial load, we
  // still have to use MT-safe cache since it could be shared by multiple
  // build contexts.
  //
  static global_cache<bool> metadata_cache;

  static optional<string>
  extract_metadata (const process_path& pp,
                    const string& key,
                    bool opt,
                    const location& loc)
  {
    if (opt)
    {
      if (metadata_cache.find (pp.effect_string ()))
        return nullopt;
    }

    // Clear current project's environment for good measure.
    //
    auto_project_env penv (nullptr);

    // Note: to ease handling (think patching third-party code) we will always
    // specify the --build2-metadata option in this single-argument form.
    //
    const char* args[] {pp.recall_string (), "--build2-metadata=1", nullptr};

    // @@ TODO This needs some more thinking/clarification. Specifically, what
    //    does it mean "x not found/not ours"? Is it just not found in PATH?
    //    That plus was not able to execute (e.g., some shared libraries
    //    missing)? That plus abnormal termination? That plus x that we found
    //    is something else?
    //
    //    Specifically, at least on Linux, when a shared library is not found,
    //    it appears exec() issues the diagnostics and calls exit(127) (that
    //    is, exec() does not return). So this is a normal termination with a
    //    peculiar exit code.
    //
    //    Overall, it feels like we should only silently ignore the "not
    //    found" and "not ours" cases since for all others the result is
    //    ambigous: it could be "ours" but just broken and the user expects
    //    us to use it but we silently ignored it. But then the same can be
    //    said about the "not ours" case: the user expected us to find "ours"
    //    but we didn't and silently ignored it.
    //
    try
    {
      // Note: not using run_*() functions since need to be able to suppress
      // all errors, including abnormal, inability to exec, etc., in case of
      // optional import. Also, no need to buffer diagnostics since in the
      // serial load.
      //
      if (verb >= 3)
        print_process (args);

      process pr (pp,
                  args,
                  -2           /* stdin  to /dev/null                 */,
                  -1           /* stdout to pipe                      */,
                  opt ? -2 : 2 /* stderr to /dev/null or pass-through */);

      try
      {
        ifdstream is (move (pr.in_ofd), ifdstream::badbit); // Note: no skip!

        // What are the odds that we will run some unrelated program which
        // will keep writing to stdout until we run out of memory reading it?
        // Apparently non-negligible (see GitHub issue #102).
        //
        string r;
        {
          char b[1024];
          while (!eof (is.read (b, sizeof (b))))
          {
            r.append (b, sizeof (b));
            if (r.size () > 65536)
            {
              is.close ();
              pr.kill ();
              pr.wait ();
              throw_generic_ios_failure (EFBIG, "output too large");
            }
          }
          r.append (b, static_cast<size_t> (is.gcount ()));
        }

        is.close (); // Detect errors.

        if (pr.wait ())
        {
          // Check the signature line. It should be in the following form:
          //
          // # build2 buildfile <key>
          //
          // This makes sure we don't treat bogus output as metadata and also
          // will allow us to support other formats (say, JSON) in the future.
          // Note that we won't be able to add more options since trying them
          // will be expensive.
          //
          // Note also that the <key> and variable prefix (as specified in the
          // export.metadata) are not necessarily the same: <key> is the
          // target name as imported. Think of it as program's canonical name,
          // for example, g++ with the actual program being g++-10, etc., and
          // the variable prefix could be gxx.
          //
          string s ("# build2 buildfile " + key);
          if (r.compare (0, s.size (), s) == 0 && r[s.size ()] == '\n')
            return r;

          if (!opt)
          {
            diag_record dr;
            dr << error (loc) << "invalid metadata signature in " << args[0]
               << " output" <<
              info << "expected '" << s << "'";

            if (verb >= 1 && verb <= 2)
            {
              dr << info << "command line: ";
              print_process (dr, args);
            }
          }

          goto fail;
        }

        // Process error, fall through.
      }
      catch (const io_error&)
      {
        // IO error (or process error), fall through.
      }

      // Deal with process or IO error.
      //
      if (pr.wait ())
      {
        if (!opt)
          error (loc) << "io error reading metadata from " << args[0];
      }
      else
      {
        // The child process presumably issued diagnostics but if it didn't,
        // the result will be very confusing. So let's issue something generic
        // for good measure. But also make it consistent with diagnostics
        // issued by run_finish().
        //
        if (!opt)
        {
          diag_record dr;
          dr << error (loc) << "unable to extract metadata from " << args[0] <<
            info << "process " << args[0] << " " << *pr.exit;

          if (verb >= 1 && verb <= 2)
          {
            dr << info << "command line: ";
            print_process (dr, args);
          }
        }
      }

      goto fail;
    }
    catch (const process_error& e)
    {
      if (!opt)
        error (loc) << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      goto fail;
    }

  fail:
    if (opt)
    {
      metadata_cache.insert (pp.effect_string (), true);
      return nullopt;
    }
    else
      throw failed ();
  }

  static void
  parse_metadata (target& t, const string& md, const location& loc)
  {
    istringstream is (md);
    path_name in ("<metadata>");

    auto df = make_diag_frame (
      [&t, &loc] (const diag_record& dr)
      {
        dr << info (loc) << "while loading metadata for " << t;
      });

    parser p (t.ctx);
    p.parse_buildfile (is, in,
                       nullptr /* root */,
                       t.base_scope ().rw (), // Load phase.
                       &t);
  }

  void
  import_suggest (const diag_record& dr,
                  const project_name& pn,
                  const target_type* tt,
                  const string& tn,
                  bool rule_hint,
                  const char* qual)
  {
    string pv (pn.variable ());

    // Suggest normal import.
    //
    dr << info << "use config.import." << pv << " configuration variable to "
       << "specify its " << (qual != nullptr ? qual : "") << "project out_root";

    // Suggest ad hoc import but only if it's a path-based target (doing it
    // for lib{} is very confusing).
    //
    if (tt != nullptr && tt->is_a<path_target> ())
    {
      string v (tt->is_a<exe> () && (pv == tn || pn == tn)
                ? "config." + pv
                : "config.import." + pv + '.' + tn + '.' + tt->name);

      dr << info << "or use " << v << " configuration variable to specify "
         << "its " << (qual != nullptr ? qual : "") << "path";
    }

    if (rule_hint)
      dr << info << "or use rule_hint attribute to specify a rule that can "
         << "find this target";
  }

  // Return the processed target name as well as the project directory, if
  // any.
  //
  // Absent project directory means nothing importable for this target was
  // found (and the returned target name is the same as the original). Empty
  // project directory means the target was found in an ad hoc manner, outside
  // of any project (in which case it may still be qualified; see
  // config.import.<proj>.<name>[.<type>]).
  //
  // Return empty name if an ad hoc import resulted in a NULL target (only
  // allowed if optional is true).
  //
  // Note that this function has a side effect of potentially marking some
  // config.import.* variables as used.
  //
  pair<name, optional<dir_path>>
  import_search (bool& new_value,
                 scope& ibase,
                 name tgt,
                 bool opt,
                 const optional<string>& meta,
                 bool subp,
                 const location& loc,
                 const char* what)
  {
    tracer trace ("import_search");

    context& ctx (ibase.ctx);
    scope& iroot (*ibase.root_scope ());

    // Depending on the target, we have four cases:
    //
    // 1. Ad hoc import: target is unqualified and is either absolute or is a
    //    directory.
    //
    //    Note: if one needs a project-local import of a relative directory
    //    (e.g., because they don't know where it is), then they will have to
    //    specify it with an explicit dir{} target type.
    //
    // 2. Project-local import: target is unqualified or the project name is
    //    the same as the importing project's.
    //
    // 3. Project-less import: target is empty-qualified.
    //
    // 4. Normal import.
    //
    // @@ PERF: in quite a few places (local, subproject) we could have
    //          returned the scope and save on bootstrap in import_load().
    //
    if (tgt.unqualified ())
    {
      if (tgt.directory () && tgt.relative ())
        tgt.dir = ibase.src_path () / tgt.dir;

      if (tgt.absolute ())
      {
        // Ad hoc import.
        //
        // Actualize the directory to be analogous to the config.import.<proj>
        // case (which is of abs_dir_path type).
        //
        tgt.dir.normalize (true /* actualize */);
        return make_pair (move (tgt), optional<dir_path> (tgt.dir));
      }
      else
      {
        // Project-local import.
        //
        const project_name& pn (project (iroot));

        if (pn.empty ())
          fail (loc) << "project-local importation of target " << tgt
                     << " from an unnamed project";

        tgt.proj = pn; // Reduce to normal import.

        return make_pair (move (tgt), optional<dir_path> (iroot.out_path ()));
      }
    }

    // If the project name is empty then we simply return it as is to let
    // someone else (e.g., a rule, import phase 2) take a stab at it.
    //
    if (tgt.proj->empty ())
      return make_pair (move (tgt), optional<dir_path> ());

    // Specifying an absolute directory in any import other than ad hoc and
    // maybe project-less does not make sense.
    //
    if (tgt.absolute ())
      fail (loc) << "absolute directory in imported target " << tgt;

    // Get the project name and convert the target to unqualified.
    //
    project_name proj (move (*tgt.proj));
    tgt.proj = nullopt;

    // Figure out the imported project's out_root.
    //
    optional<dir_path> out_root;

    // First try the config.import.* mechanism. The idea is that if the user
    // explicitly told us the project's location, then we should prefer that
    // over anything that we may discover. In particular, we will prefer it
    // over any bundled subprojects.
    //
    // Note: go straight for the public variable pool.
    //
    auto& vp (iroot.var_pool (true /* public */));

    using config::lookup_config;

    for (;;) // Break-out loop.
    {
      string projv (proj.variable ());
      string n ("config.import." + projv);

      // Skip import phase 1.
      //
      auto skip = [&tgt, &proj, &trace] ()
      {
        tgt.proj = move (proj);
        l5 ([&]{trace << "skipping " << tgt;});
        return make_pair (move (tgt), optional<dir_path> ());
      };

      // Add hoc import.
      //
      // config.import.<proj>.<name>.<type>
      // config.import.<proj>.<name>
      //
      // For example: config.import.build2.b.exe=/opt/build2/bin/b
      //
      // If <type> is exe and <proj> and <name> are the same, then we also
      // recognize the special config.<proj> (tool importation; we could
      // also handle the case where <proj> is not the same as <name> via
      // the config.<proj>.<name> variable). For backwards-compatibility
      // reasons, it takes precedence over config.import.
      //
      // Note: see import phase 2 diagnostics if changing anything here.
      //
      // @@ How will this work for snake-case targets, say libs{build2-foo}?
      //    As well as for dot-separated target types, say, cli.cxx{}?
      //
      // @@ This duality has a nasty side-effect: if we have config.<proj>
      //    configured, then specifying config.<proj>.import has no effect
      //    (see also a note below on priority just among these options).
      //
      //    Some ideas on how to resolve this include: using lookup depth,
      //    using override info, and using the "new value" status. All of
      //    these undoubtfully will complicate this logic (i.e., we will have
      //    to lookup all of them and then decide which one "wins").
      //
      if (!tgt.value.empty ())
      {
        // Return NULL if not found and empty path if NULL. For executable
        // targets (exe is true), also treat the special `false` value as
        // NULL.
        //
        auto lookup = [&new_value, &iroot, opt, &loc, what] (
          const variable& var, bool exe) -> const path*
        {
          auto l (lookup_config (new_value, iroot, var));

          if (l.defined ())
          {
            const path* p (cast_null<path> (l));

            if (p != nullptr)
            {
              if (p->empty ())
                fail (loc) << "empty path in " << var;

              if (!exe || p->to_directory () || p->string () != "false")
                return p;
            }

            if (!opt)
              fail (loc) << (p == nullptr ? "null" : "false") << " in "
                         << var << " for non-optional " << what;

            return &empty_path;
          }

          return nullptr;
        };

        // First try config.<proj>, then import.<name>.<type>, and finally
        // just import.<name>.
        //
        // @@ What should we do if several of them are specified? For example,
        //    one is inherited from amalgamation while the other is specified
        //    on the project's root? We could pick the one with the least
        //    lookup depth. On the other hand, we expect people to stick with
        //    the config.<proj> notation for tools (since it's a lot easier to
        //    type) so let's not complicate things for the time being.
        //
        //    Another alternative would be to see which one is new.
        //
        const path* p (nullptr);

        if (tgt.typed ())
        {
          bool e (tgt.type == "exe");

          // The config.import.* vars are pattern-typed in context ctor as an
          // overridable variable of type path. The config.<proj> we have to
          // type manually.
          //
          if (e && (projv == tgt.value || proj == tgt.value))
            p = lookup (vp.insert<path> ("config." + projv), e);

          if (p == nullptr)
            p = lookup (vp.insert (n + '.' + tgt.value + '.' + tgt.type), e);
        }

        if (p == nullptr)
          p = lookup (vp.insert (n + '.' + tgt.value), false);

        if (p != nullptr)
        {
          if (p->empty ())
            tgt = name (); // NULL
          else
          {
            string on (move (tgt.value)); // Original name as imported.

            tgt.dir = p->directory ();
            tgt.value = p->leaf ().string ();

            // If the path is relative, then keep it project-qualified
            // assuming import phase 2 knows what to do with it. Think:
            //
            // config.import.build2.b=b-boot
            //
            // @@ Maybe we should still complete it if it's not simple? After
            //    all, this is a path, do we want interpretations other than
            //    relative to CWD? Maybe we do, who knows. Doesn't seem to
            //    harm anything at the moment.
            //
            // Why not call import phase 2 directly here? Well, one good
            // reason would be to allow for rule-specific import resolution.
            //
            if (p->relative ())
              tgt.proj = move (proj);
            else
            {
              // Enter the target and assign its path (this will most commonly
              // be some out of project file).
              //
              // @@ Should we check that the file actually exists (and cache
              //    the extracted timestamp)? Or just let things take their
              //    natural course?
              //
              name n (tgt);
              const target_type* tt (ibase.find_target_type (n, loc).first);

              if (tt == nullptr)
                fail (loc) << "unknown target type " << n.type << " in " << n;

              // Note: not using the extension extracted by find_target_type()
              // to be consistent with import phase 2.
              //
              target& t (insert_target (trace, ctx, *tt, *p).first);

              // Load the metadata, similar to import phase 2.
              //
              if (meta)
              {
                if (exe* e = t.is_a<exe> ())
                {
                  if (!e->vars[ctx.var_export_metadata].defined ())
                  {
                    optional<string> md;
                    {
                      auto df = make_diag_frame (
                        [&proj, tt, &on] (const diag_record& dr)
                        {
                          import_suggest (
                            dr, proj, tt, on, false, "alternative ");
                        });

                      md = extract_metadata (e->process_path (),
                                             *meta,
                                             false /* optional */,
                                             loc);
                    }

                    parse_metadata (*e, move (*md), loc);
                  }
                }
              }
            }
          }

          return make_pair (move (tgt), optional<dir_path> (dir_path ()));
        }
      }

      // Normal import.
      //
      // config.import.<proj>
      //
      // Note: see import phase 2 diagnostics if changing anything here.
      //
      {
        // Note: pattern-typed in context ctor as an overridable variable of
        // type abs_dir_path (path auto-completion).
        //
        auto l (lookup_config (new_value, iroot, vp.insert (n)));

        if (l.defined ())
        {
          const dir_path* d (cast_null<dir_path> (l));

          // Empty/NULL config.import.* value means don't look in subprojects
          // or amalgamations and go straight to the rule-specific import
          // (e.g., to use system-installed).
          //
          if (d == nullptr || d->empty ())
            return skip ();

          out_root = *d; // Normalized and actualized.
          break;
        }
      }

      // import.build2
      //
      // Note that the installed case is taken care of by special code in the
      // cc module's search_library().
      //
      if (proj == "build2")
      {
        // Note that this variable can be set to NULL to disable relying on
        // the built-in path. We use this in our tests to make sure we are
        // importing and testing the build system being built and not the one
        // doing the building.
        //
        if (auto l = iroot[ctx.var_import_build2])
        {
          out_root = cast<dir_path> (l);

          if (out_root->empty ())
            return skip ();

          break;
        }
      }

      // Otherwise search subprojects, starting with our root and then trying
      // outer roots for as long as we are inside an amalgamation.
      //
      if (subp)
      {
        for (scope* r (&iroot);; r = r->parent_scope ()->root_scope ())
        {
          l5 ([&]{trace << "looking in " << *r;});

          // First check the amalgamation itself.
          //
          if (r != &iroot && project (*r) == proj)
          {
            out_root = r->out_path ();
            break;
          }

          if (const subprojects* ps = *r->root_extra->subprojects)
          {
            auto i (ps->find (proj));
            if (i != ps->end ())
            {
              const dir_path& d ((*i).second);
              out_root = r->out_path () / d;
              break;
            }
          }

          if (!r->vars[ctx.var_amalgamation])
            break;
        }
      }

      break;
    }

    // Add the qualification back to the target (import_load() will remove it
    // again).
    //
    tgt.proj = move (proj);

    return make_pair (move (tgt), move (out_root));
  }

  pair<names, const scope&>
  import_load (context& ctx,
               pair<name, optional<dir_path>> x,
               bool meta,
               const location& loc)
  {
    tracer trace ("import_load");

    uint64_t metav (meta ? 1 : 0); // Metadata version.

    // We end up here in two cases: Ad hoc import, in which case name is
    // unqualified and absolute and path is a base, not necessarily root. And
    // normal import, in which case name must be project-qualified and path is
    // a root.
    //
    assert (x.second);
    name tgt (move (x.first));
    optional<project_name> proj;

    if (tgt.qualified ())
    {
      assert (tgt.proj);

      proj = move (*tgt.proj);
      tgt.proj = nullopt;
    }
    else
      assert (tgt.absolute ());

    // Bootstrap the imported root scope. This is pretty similar to what we do
    // in main() except that here we don't try to guess src_root.
    //
    // For the normal import the user can also specify the out_root of the
    // amalgamation that contains our project. For now we only consider
    // top-level sub-projects.
    //
    scope* root;
    dir_path out_root, src_root;

    // See if this is a forwarded configuration. For top-level project we want
    // to use the same logic as in main() while for inner subprojects -- as in
    // create_bootstrap_inner().
    //
    bool fwd (false);
    optional<bool> altn;
    {
      bool src;
      if (proj)
      {
        out_root = move (*x.second);
        src = is_src_root (out_root, altn);
      }
      else
      {
        // For ad hoc import, find our root.
        //
        pair<dir_path, bool> p (find_out_root (*x.second, altn));
        out_root = move (p.first);
        src = p.second;

        if (out_root.empty ())
          fail (loc) << "no project for imported target " << tgt;
      }

      if (src)
      {
        src_root = move (out_root);
        out_root = bootstrap_fwd (ctx, src_root, altn);
        fwd = (src_root != out_root);
      }
    }

    // First check the cache.
    //
    using import_key = context::import_key;

    auto cache_find = [&ctx, &tgt, metav] (dir_path& out_root) ->
      const pair<names, const scope&>*
    {
      import_key k {move (out_root), move (tgt), metav};

      auto i (ctx.import_cache.find (k));
      if (i != ctx.import_cache.end ())
        return &i->second;

      out_root = move (k.out_root);
      tgt = move (k.target);

      return nullptr;
    };

    if (proj)
    {
      if (const auto* r = cache_find (out_root))
        return *r;
    }

    dir_path cache_out_root;

    // Clear current project's environment.
    //
    auto_project_env penv (nullptr);

    // Note: this loop does at most two iterations.
    //
    for (const scope* proot (nullptr); ; proot = root)
    {
      bool top (proot == nullptr);

      // Check the cache for the subproject.
      //
      if (!top && proj)
      {
        if (const auto* r = cache_find (out_root))
          return *r;
      }

      root = create_root (ctx, out_root, src_root)->second.front ();

      bool bstrapped (bootstrapped (*root));

      if (!bstrapped)
      {
        value& v (bootstrap_out (*root, altn));

        // Check that the bootstrap process set src_root.
        //
        if (v)
        {
          // Note that unlike main() here we fail hard. The idea is that if
          // the project we are importing is misconfigured, then it should be
          // fixed first.
          //
          const dir_path& p (cast<dir_path> (v));

          if (!src_root.empty () && p != src_root)
            fail (loc) << "configured src_root " << p << " does not match "
                       << "discovered " << src_root;
        }
        else
        {
          diag_record dr;
          dr << fail (loc) << "unable to determine src_root for imported ";
          if (proj)
            dr << *proj;
          else
            dr << out_root;
          dr << info << "consider configuring " << out_root;
        }

        setup_root (*root,
                    (top
                     ? fwd
                     : forwarded (*proot, out_root, v.as<dir_path> (), altn)));

        bootstrap_pre (*root, altn);
        bootstrap_src (*root, altn);
        if (!top)
          bootstrap_post (*root);
      }
      else
      {
        altn = root->root_extra->altn;

        if (src_root.empty ())
          src_root = root->src_path ();

        if (top ? fwd : forwarded (*proot, out_root, src_root, altn))
          root->assign (ctx.var_forwarded) = true; // Only upgrade (see main()).
      }

      if (top)
      {
        create_bootstrap_outer (*root);

        if (!bstrapped)
          bootstrap_post (*root);
      }

      // If this is ad hoc import, then we are done.
      //
      if (!proj)
        break;

      // Now we know this project's name as well as all its subprojects.
      //
      if (project (*root) == *proj)
        break;

      if (const subprojects* ps = *root->root_extra->subprojects)
      {
        auto i (ps->find (*proj));

        if (i != ps->end ())
        {
          cache_out_root = move (out_root);

          const dir_path& d ((*i).second);
          altn = nullopt;
          out_root = root->out_path () / d;
          src_root = is_src_root (out_root, altn) ? out_root : dir_path ();
          continue;
        }
      }

      fail (loc) << out_root << " is not out_root for " << *proj;
    }

    // Buildfile importation is quite different so handle it separately.
    //
    // Note that we don't need to load the project in this case.
    //
    // @@ For now we don't out-qualify the resulting target to be able to
    //    re-import it ad hoc (there is currently no support for out-qualified
    //    ad hoc import). Feels like this should be harmless since it's just a
    //    glorified path to a static file that nobody is actually going to use
    //    as a target (e.g., to depend upon).
    //
    if (tgt.type == "buildfile")
    {
      auto add_ext = [&altn] (string& n)
      {
        if (path_traits::find_extension (n) == string::npos)
        {
          if (n != (*altn ? alt_buildfile_file : std_buildfile_file).string ())
          {
            n += ".";
            n += *altn ? alt_build_ext : std_build_ext;
          }
        }
      };

      if (proj)
      {
        name n;

        if (src_root.empty ())
          src_root = root->src_path ();

        n.dir = move (src_root);
        n.dir /= *altn ? alt_export_dir : std_export_dir;
        if (!tgt.dir.empty ())
        {
          n.dir /= tgt.dir;
          n.dir.normalize ();
        }

        n.type = tgt.type;
        n.value = tgt.value;
        add_ext (n.value);

        pair<names, const scope&> r (names {move (n)}, *root);

        // Cache.
        //
        if (cache_out_root.empty ())
          cache_out_root = move (out_root);

        ctx.import_cache.emplace (
          import_key {move (cache_out_root), move (tgt), metav}, r);

        return r;
      }
      else
      {
        add_ext (tgt.value);
        return pair<names, const scope&> (names {move (tgt)}, *root);
      }
    }

    // Load the imported root scope.
    //
    if (!root->root_extra->loaded)
      load_root (*root);

    // If this is a normal import, then we go through the export stub.
    //
    if (proj)
    {
      scope& gs (ctx.global_scope.rw ());

      // Use a temporary scope so that the export stub doesn't mess anything
      // up.
      //
      temp_scope ts (gs);

      // "Pass" the imported project's roots to the stub.
      //
      if (cache_out_root.empty ())
        cache_out_root = out_root;

      if (src_root.empty ())
        src_root = root->src_path ();

      ts.assign (ctx.var_out_root) = move (out_root);
      ts.assign (ctx.var_src_root) = move (src_root);

      // Pass the target being imported in import.target.
      //
      {
        value& v (ts.assign (ctx.var_import_target));

        if (!tgt.empty ()) // Otherwise leave NULL.
          v = tgt; // Can't move (need for diagnostics below).
      }

      // Pass the metadata compatibility version in import.metadata.
      //
      if (meta)
        ts.assign (ctx.var_import_metadata) = metav;

      // Load the export stub. Note that it is loaded in the context of the
      // importing project, not the imported one. The export stub will
      // normally switch to the imported root scope at some point.
      //
      path es (root->src_path () / root->root_extra->export_file);

      try
      {
        ifdstream ifs (es);

        l5 ([&]{trace << "importing " << es;});

        // @@ Should we verify these are all unqualified names? Or maybe there
        // is a use-case for the export stub to return a qualified name? E.g.,
        // re-export?
        //
        names v;
        {
          auto df = make_diag_frame (
            [&tgt, &loc] (const diag_record& dr)
            {
              dr << info (loc) << "while loading export stub for " << tgt;
            });

          parser p (ctx);
          v = p.parse_export_stub (ifs, path_name (es), *root, gs, ts);
        }

        // If there were no export directive executed in an export stub,
        // assume the target is not exported.
        //
        if (v.empty () && !tgt.empty ())
          fail (loc) << "target " << tgt << " is not exported by project "
                     << *proj;

        pair<names, const scope&> r (move (v), *root);

        // Cache.
        //
        ctx.import_cache.emplace (
          import_key {move (cache_out_root), move (tgt), metav}, r);

        return r;
      }
      catch (const io_error& e)
      {
        fail (loc) << "unable to read buildfile " << es << ": " << e << endf;
      }
    }
    else
    {
      // In case of an ad hoc import we need to load a buildfile that can
      // plausibly define this target. We use the same hairy semantics as in
      // main() (and where one should refer for details).
      //
      const dir_path& src_root (root->src_path ());
      dir_path src_base (x.second->sub (src_root)
                         ? move (*x.second)
                         : src_out (*x.second, *root));

      optional<path> bf (find_buildfile (src_base, src_base, altn));

      if (!bf)
      {
        bf = find_plausible_buildfile (tgt, *root,
                                       src_base, src_root,
                                       altn);
        if (!bf)
          fail << "no buildfile in " << src_base << " or parent directories "
               << "for imported target " << tgt;

        if (!bf->empty ())
          src_base = bf->directory ();
      }

      // Load the buildfile unless it is implied.
      //
      if (!bf->empty ())
      {
        // The same logic as in operation's load().
        //
        dir_path out_base (out_src (src_base, *root));

        auto i (ctx.scopes.rw (*root).insert_out (out_base));
        scope& base (setup_base (i, move (out_base), move (src_base)));

        source_once (*root, base, *bf);
      }

      // If this is forwarded src, then remap the target to out (will need to
      // adjust this if/when we allow out-qualification).
      //
      if (fwd)
        tgt.dir = out_src (tgt.dir, *root);

      return pair<names, const scope&> (names {move (tgt)}, *root);
    }
  }

  const target_type&
  import_target_type (scope& root,
                      const scope& iroot, const string& n,
                      const location& l)
  {
    // NOTE: see similar code in parser::parse_define().

    const target_type* tt (iroot.find_target_type (n));
    if (tt == nullptr)
      fail (l) << "unknown imported target type " << n << " in project "
               << iroot;

    auto p (root.root_extra->target_types.insert (*tt));

    if (!p.second && &p.first.get () != tt)
      fail (l) << "imported target type " << n << " already defined in project "
               << root;

    return *tt;
  }

  static names
  import2_buildfile (context&, names&&, bool, const location&);

  static const target*
  import2 (context&, const scope&, names&,
           const string&, bool, const optional<string>&, bool,
           const location&);

  import_result<scope>
  import (scope& base,
          name tgt,
          const optional<string>& ph2,
          bool opt,
          bool metadata,
          const location& loc)
  {
    tracer trace ("import");

    l5 ([&]{trace << tgt << " from " << base;});

    assert ((!opt || ph2) && (!metadata || ph2));

    context& ctx (base.ctx);
    assert (ctx.phase == run_phase::load);

    // Validate the name.
    //
    if (tgt.qualified () && tgt.empty ())
      fail (loc) << "project-qualified empty name " << tgt;

    // If metadata is requested, delegate to import_direct() which will lookup
    // the target and verify the metadata was loaded.
    //
    if (metadata)
    {
      import_result<target> r (
        import_direct (base, move (tgt), ph2, opt, metadata, loc));

      return import_result<scope> {
        r.target != nullptr ? r.target->base_scope ().root_scope () : nullptr,
        move (r.name),
        r.kind};
    }

    pair<name, optional<dir_path>> r (
      import_search (base,
                     move (tgt),
                     opt,
                     nullopt /* metadata */,
                     true    /* subpproj */,
                     loc));

    // If there is no project, we are either done or go straight to phase 2.
    //
    if (!r.second || r.second->empty ())
    {
      names ns;
      const target* t (nullptr);

      if (r.first.empty ())
      {
        assert (opt); // NULL
      }
      else
      {
        ns.push_back (move (r.first));

        // If the target is still qualified, it is either phase 2 now or we
        // return it as is to let someone else (e.g., a rule, import phase 2)
        // take a stab at it later.
        //
        if (ns.back ().qualified ())
        {
          if (ns.back ().type == "buildfile")
          {
            assert (ph2);
            ns = import2_buildfile (ctx, move (ns), opt && !r.second, loc);
          }
          else if (ph2)
          {
            // This is tricky: we only want the optional semantics for the
            // fallback case.
            //
            t = import2 (ctx,
                         base, ns,
                         *ph2,
                         opt && !r.second  /* optional */,
                         nullopt           /* metadata */,
                         false             /* existing */,
                         loc);

            if (t != nullptr)
            {
              // Note that here r.first was still project-qualified and we
              // have no choice but to call as_name(). This shouldn't cause
              // any problems since the import() call assigns the extension.
              //
              ns = t->as_name ();
            }
            else
              ns.clear (); // NULL
          }
          else
            l5 ([&]{trace << "postponing " << ns.back ();});
        }
      }

      return import_result<scope> {
        t != nullptr ? t->base_scope ().root_scope () : nullptr,
        move (ns),
        r.second.has_value () ? import_kind::adhoc : import_kind::fallback};
    }

    import_kind k (r.first.absolute ()
                   ? import_kind::adhoc
                   : import_kind::normal);

    pair<names, const scope&> p (
      import_load (base.ctx, move (r), false /* metadata */, loc));

    return import_result<scope> {&p.second, move (p.first), k};
  }

  const target*
  import2 (context& ctx,
           const prerequisite_key& pk,
           const string& hint,
           bool opt,
           const optional<string>& meta,
           bool exist,
           const location& loc)
  {
    tracer trace ("import2");

    // Neither hint nor metadata can be requested for existing.
    //
    assert (!exist || (!meta && hint.empty ()));

    assert (pk.proj);
    const project_name& proj (*pk.proj);

    // Note that if this function returns a target, it should have the
    // extension assigned (like the find/insert_target() functions) so that
    // as_name() returns a stable name.

    // Rule-specific resolution.
    //
    if (!hint.empty ())
    {
      assert (pk.scope != nullptr);

      // Note: similar to/inspired by match_rule_impl().
      //
      // Search scopes outwards, stopping at the project root.
      //
      for (const scope* s (pk.scope);
           s != nullptr;
           s = s->root () ? nullptr : s->parent_scope ())
      {
        // We only look for rules that are registered for perform(update).
        //
        if (const operation_rule_map* om = s->rules[perform_id])
        {
          if (const target_type_rule_map* ttm  = (*om)[update_id])
          {
            // Ignore the target type the rules are registered for (this is
            // about prerequisite types, not target).
            //
            // @@ Note that the same rule could be registered for several
            //    types which means we will keep calling it repeatedly.
            //
            for (const auto& p: *ttm)
            {
              const name_rule_map& nm (p.second);

              // Filter against the hint.
              //
              for (auto p (nm.find_sub (hint)); p.first != p.second; ++p.first)
              {
                const string& n (p.first->first);
                const rule& r (p.first->second);

                auto df = make_diag_frame (
                  [&pk, &n](const diag_record& dr)
                  {
                    if (verb != 0)
                      dr << info << "while importing " << pk << " using rule "
                         << n;
                  });

                if (const target* t = r.import (pk, meta, loc))
                  return t;
              }
            }
          }
        }
      }
    }

    // Builtin resolution for certain target types.
    //
    const target_key& tk (pk.tk);
    const target_type& tt (*tk.type);

    // Try to find the executable in PATH (or CWD if relative).
    //
    for (; tt.is_a<exe> (); ) // Breakout loop.
    {
      path n (*tk.dir);
      n /= *tk.name;
      if (tk.ext)
      {
        n += '.';
        n += *tk.ext;
      }

      // Only search in PATH (or CWD if not simple).
      //
      process_path pp (
        process::try_path_search (n,
                                  false       /* init */,
                                  dir_path () /* fallback */,
                                  true        /* path_only */));
      if (pp.empty ())
        break;

      const path& p (pp.effect);
      assert (!p.empty ()); // We searched for a relative path.

      if (exist) // Note: then meta is false.
      {
        if (const target* t = find_target (trace, ctx, tt, p))
          return t;

        break;
      }

      // Try hard to avoid re-extracting the metadata (think of a tool that is
      // used by multiple projects in an amalgamation).
      //
      optional<string> md;
      optional<const target*> t;
      if (meta)
      {
        t = find_target (trace, ctx, tt, p);

        if (*t != nullptr && (*t)->vars[ctx.var_export_metadata].defined ())
          return *t; // We've got all we need.

        auto df = make_diag_frame (
          [&proj, &tt, &tk] (const diag_record& dr)
          {
            import_suggest (dr, proj, &tt, *tk.name, false, "alternative ");
          });

        if (!(md = extract_metadata (pp, *meta, opt, loc)))
          break;
      }

      if (!t || *t == nullptr)
      {
        // Note: we need the lock because process_path() call below is not
        // MT-safe.
        //
        pair<target&, ulock> r (insert_target (trace, ctx, tt, p));
        t = &r.first;

        // Cache the process path if we've created the target (it's possible
        // that the same target will be imported via different paths, e.g., as
        // a simple name via PATH search and as an absolute path in which case
        // the first import will determine the path).
        //
        if (r.second)
          r.first.as<exe> ().process_path (move (pp));
      }

      // Save the metadata. Note that this happens during the load phase and
      // so MT-safe.
      //
      if (meta)
        parse_metadata ((*t)->rw (), *md, loc);

      return *t;
    }

    // NOTE: see similar code in import2() below if changing anything here.

    if (opt || exist)
      return nullptr;

    diag_record dr;
    dr << fail (loc) << "unable to import target " << pk;

    if (proj.empty ())
      dr << info << "consider adding its installation location" <<
        info << "or explicitly specify its project name";
    else
      // Use metadata as proxy for immediate import.
      //
      import_suggest (dr, proj, &tt, *tk.name, meta && hint.empty ());

    dr << endf;
  }

  // As above but with scope/ns instead of pk. This version deals with the
  // unknown target type case.
  //
  static const target*
  import2 (context& ctx,
           const scope& base, names& ns,
           const string& hint,
           bool opt,
           const optional<string>& meta,
           bool exist,
           const location& loc)
  {
    // If we have a rule hint, then it's natural to expect this target type is
    // known to the importing project. Ditto for project-less import.
    //
    const target_type* tt (nullptr);
    if (hint.empty ())
    {
      size_t n;
      if ((n = ns.size ()) != 0 && n == (ns[0].pair ? 2 : 1))
      {
        const name& n (ns.front ());

        if (n.typed () && !n.proj->empty ())
        {
          tt = base.find_target_type (n.type);

          if (tt == nullptr)
          {
            // A subset of code in the above version of import2().
            //
            if (opt || exist)
              return nullptr;

            diag_record dr;
            dr << fail (loc) << "unable to import target " << ns;
            import_suggest (dr, *n.proj, nullptr, string (), meta.has_value ());
          }
        }
      }
    }

    return import2 (ctx,
                    base.find_prerequisite_key (ns, loc, tt),
                    hint,
                    opt,
                    meta,
                    exist,
                    loc);
  }

  static names
  import2_buildfile (context&, names&& ns, bool opt, const location& loc)
  {
    tracer trace ("import2_buildfile");

    assert (ns.size () == 1);
    name n (move (ns.front ()));

    // Our approach doesn't work for targets without a project so let's fail
    // hard, even if optional.
    //
    if (!n.proj || n.proj->empty ())
      fail (loc) << "unable to import target " << n << " without project name";

    while (!build_install_buildfile.empty ()) // Breakout loop.
    {
      path f (build_install_buildfile      /
              dir_path (n.proj->string ()) /
              n.dir                        /
              n.value);

      // See if we need to try with extensions.
      //
      bool ext (path_traits::find_extension (n.value) == string::npos &&
                n.value != std_buildfile_file.string () &&
                n.value != alt_buildfile_file.string ());

      if (ext)
      {
        f += '.';
        f += std_build_ext;
      }

      if (!exists (f))
      {
        l6 ([&]{trace << "tried " << f;});

        if (ext)
        {
          f.make_base ();
          f += '.';
          f += alt_build_ext;

          if (!exists (f))
          {
            l6 ([&]{trace << "tried " << f;});
            break;
          }
        }
        else
          break;
      }

      // Split the path into the target.
      //
      ns = {name (f.directory (), move (n.type), f.leaf ().string ())};
      return move (ns);
    }

    if (opt)
      return names {};

    diag_record dr;
    dr << fail (loc) << "unable to import target " << n;

    import_suggest (dr, *n.proj, nullptr /* tt */, n.value, false);

    if (build_install_buildfile.empty ())
      dr << info << "no exported buildfile installation location is "
         << "configured in build2";
    else
      dr << info << "exported buildfile installation location is "
         << build_install_buildfile;

    dr << endf;
  }

  import_result<target>
  import_direct (bool& new_value,
                 scope& base,
                 name tgt,
                 const optional<string>& ph2,
                 bool opt,
                 bool metadata,
                 const location& loc,
                 const char* what)
  {
    // This is like normal import() except we return the target in addition to
    // its name.
    //
    tracer trace ("import_direct");

    l5 ([&]{trace << tgt << " from " << base << " for " << what;});

    assert ((!opt || ph2) && (!metadata || ph2) && tgt.type != "buildfile");

    context& ctx (base.ctx);
    assert (ctx.phase == run_phase::load);

    scope& root (*base.root_scope ());

    // Use the original target name as metadata key.
    //
    auto meta (metadata ? optional<string> (tgt.value) : nullopt);

    names ns, rns;
    import_kind k;
    const target* pt (nullptr);
    const scope* iroot (nullptr); // Imported root scope.

    // Original project/name as imported for diagnostics.
    //
    string oname (meta ? tgt.value : string ());
    project_name oproj (meta && tgt.proj ? *tgt.proj : project_name ());

    pair<name, optional<dir_path>> r (
      import_search (new_value,
                     base,
                     move (tgt),
                     opt,
                     meta,
                     true /* subpproj */,
                     loc,
                     what));

    // If there is no project, we are either done or go straight to phase 2.
    //
    if (!r.second || r.second->empty ())
    {
      k = r.second.has_value () ? import_kind::adhoc : import_kind::fallback;

      if (r.first.empty ())
      {
        assert (opt);
        return import_result<target> {nullptr, {}, k}; // NULL
      }
      else if (r.first.qualified ())
      {
        if (ph2)
        {
          names ns {move (r.first)};

          // This is tricky: we only want the optional semantics for the
          // fallback case.
          //
          pt = import2 (ctx,
                        base, ns,
                        *ph2,
                        opt && !r.second,
                        meta,
                        false /* existing */,
                        loc);
        }

        if (pt == nullptr)
          return import_result<target> {nullptr, {}, k}; // NULL

        // Note that here r.first was still project-qualified and we have no
        // choice but to call as_name() (below). This shouldn't cause any
        // problems since the import() call assigns the extension.

        // Fall through.
      }
      else
      {
        // It's a bit fuzzy in which cases we end up here. So for now we keep
        // the original if it's absolute and call as_name() otherwise.
        //
        // @@ TODO: resolve iroot or assume target type should be known?
        //
        if (r.first.absolute ())
          rns.push_back (r.first);

        ns.push_back (move (r.first)); // And fall through.
      }
    }
    else
    {
      k = r.first.absolute () ? import_kind::adhoc : import_kind::normal;

      pair<names, const scope&> p (
        import_load (base.ctx, move (r), metadata, loc));

      rns = ns = move (p.first);
      iroot = &p.second;
    }

    if (pt == nullptr)
    {
      // Import (more precisely, alias) the target type into this project
      // if not known.
      //
      const target_type* tt (nullptr);
      if (iroot != nullptr && !ns.empty ())
      {
        const name& n (ns.front ());
        if (n.typed ())
          tt = &import_target_type (root, *iroot, n.type, loc);
      }

      // Similar logic to perform's search(). Note: modifies ns.
      //
      target_key tk (base.find_target_key (ns, loc, tt));
      pt = ctx.targets.find (tk, trace);
      if (pt == nullptr)
        fail (loc) << "unknown imported target " << tk;
    }

    if (rns.empty ())
      rns = pt->as_name ();

    target& t (pt->rw ()); // Load phase.

    // Note that if metadata is requested via any of the import*() functions,
    // then we will always end up here (see delegates to import_direct()),
    // which is where we do the final verifications and processing.
    //
    if (meta)
    {
      auto df = make_diag_frame (
        [&oproj, &oname, &t] (const diag_record& dr)
        {
          if (!oproj.empty ())
            import_suggest (dr, oproj, &t.type (), oname, false, "alternative ");
        });

      // The export.metadata value should start with the version followed by
      // the metadata variable prefix.
      //
      // Note: lookup on target, not target::vars since it could come from
      // the group (think lib{} metadata).
      //
      lookup l (t[ctx.var_export_metadata]);
      if (l && !l->empty ())
      {
        const names& ns (cast<names> (l));

        // First verify the version.
        //
        uint64_t ver;
        try
        {
          // Note: does not change the passed name.
          //
          ver = value_traits<uint64_t>::convert (
            ns[0], ns[0].pair ? &ns[1] : nullptr);
        }
        catch (const invalid_argument& e)
        {
          fail (loc) << "invalid metadata version in imported target " << t
                     << ": " << e << endf;
        }

        if (ver != 1)
          fail (loc) << "unexpected metadata version " << ver
                     << " in imported target " << t;

        // Next verify the metadata variable prefix.
        //
        if (ns.size () != 2 || !ns[1].simple ())
          fail (loc) << "invalid metadata variable prefix in imported "
                     << "target " << t;

        const string& pfx (ns[1].value);

        // See if we have the stable program name in the <var-prefix>.name
        // variable. If its missing, set it to the metadata key (i.e., target
        // name as imported) by default.
        //
        {
          // Note: go straight for the public variable pool.
          //
          auto& vp (ctx.var_pool.rw ()); // Load phase.

          value& nv (t.assign (vp.insert (pfx + ".name")));
          if (!nv)
            nv = *meta;
        }

        // See if the program reported the use of environment variables and
        // if so save them as affecting this project.
        //
        if (const auto* e = cast_null<strings> (t.vars[pfx + ".environment"]))
        {
          for (const string& v: *e)
            config::save_environment (root, v);
        }
      }
      else
        fail (loc) << "no metadata for imported target " << t;
    }

    return import_result<target> {pt, move (rns), k};
  }

  path
  import_buildfile (scope& bs, name n, bool opt, const location& loc)
  {
    names r (import (bs,
                     move (n),
                     string () /* phase2 */,
                     opt,
                     false     /* metadata */,
                     loc).name);

    path p;
    if (!r.empty ()) // Optional not found.
    {
      // Note: see also parse_import().
      //
      assert (r.size () == 1); // See import_load() for details.
      name& n (r.front ());
      p = n.dir / n.value; // Should already include extension.
    }
    else
      assert (opt);

    return p;
  }

  ostream&
  operator<< (ostream& o, const import_result<exe>& r)
  {
    assert (r.target != nullptr);

    if (r.kind == import_kind::normal)
      o << *r.target;
    else
      o << r.target->process_path ();

    return o;
  }

  void
  create_project (const dir_path& d,
                  const optional<dir_path>& amal,
                  const strings& bmod,
                  const string&  rpre,
                  const strings& rmod,
                  const string&  rpos,
                  const optional<string>& config_mod,
                  const optional<string>& config_file,
                  bool buildfile,
                  const char* who,
                  uint16_t verbosity)
  {
    assert (!config_file || (config_mod && *config_mod == "config"));

    string hdr ("# Generated by " + string (who) + ". Edit if you know"
                " what you are doing.\n"
                "#");

    // If the directory exists, verify it's empty. Otherwise, create it.
    //
    if (exists (d))
    {
      if (!empty (d))
        fail << "directory " << d << " exists and is not empty";
    }
    else
      mkdir_p (d, verbosity);

    // Create the build/ subdirectory.
    //
    // Note that for now we use the standard build file/directory scheme.
    //
    mkdir (d / std_build_dir, verbosity);

    auto diag = [verbosity] (const path& f)
    {
      if (verb >= verbosity)
      {
        if (verb >= 2)
          text << "cat >" << f;
        else if (verb)
          print_diag ("save", f);
      }
    };

    // Write build/bootstrap.build.
    //
    {
      path f (d / std_bootstrap_file);

      diag (f);

      try
      {
        ofdstream ofs (f);

        ofs << hdr << endl
            << "project =" << endl;

        if (amal)
        {
          ofs << "amalgamation =";

          if (!amal->empty ())
          {
            ofs << ' ';
            to_stream (ofs, *amal, true /* representation */);
          }

          ofs << endl;
        }

        ofs << endl;

        if (config_mod)
          ofs << "using " << *config_mod << endl;

        for (const string& m: bmod)
        {
          if (!config_mod || m != *config_mod)
            ofs << "using " << m << endl;
        }

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << f << ": " << e;
      }
    }

    // Write build/root.build.
    //
    {
      path f (d / std_root_file);

      diag (f);

      try
      {
        ofdstream ofs (f);

        ofs << hdr << endl;

        if (!rpre.empty ())
          ofs << rpre << endl
              << endl;

        for (const string& cm: rmod)
        {
          // If the module name start with '?', then use optional load.
          //
          bool opt (cm.front () == '?');
          string m (cm, opt ? 1 : 0);

          // Append .config unless the module name ends with '.', in which
          // case strip it.
          //
          if (m.back () == '.')
            m.pop_back ();
          else
            m += ".config";

          ofs << "using" << (opt ? "?" : "") << " " << m << endl;
        }

        if (!rpos.empty ())
          ofs << endl
              << rpre << endl;

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << f << ": " << e;
      }
    }

    // Write build/config.build.
    //
    if (config_file)
    {
      path f (d / std_build_dir / "config.build"); // std_config_file

      diag (f);

      try
      {
        ofdstream ofs (f);

        ofs << hdr << endl
            << "config.version = " << config::module::version << endl
            << endl
            << *config_file << endl;

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << f << ": " << e;
      }
    }

    // Write root buildfile.
    //
    if (buildfile)
    {
      path f (d / std_buildfile_file);

      diag (f);

      try
      {
        ofdstream ofs (f);

        ofs << hdr << endl
            << "./: {*/ -build/}" << endl;

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << f << ": " << e;
      }
    }
  }
}
