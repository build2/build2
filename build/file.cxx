// file      : build/file.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/file>

#include <map>
#include <fstream>
#include <utility> // move()

#include <butl/filesystem>

#include <build/scope>
#include <build/context>
#include <build/parser>
#include <build/diagnostics>

#include <build/token>
#include <build/lexer>

using namespace std;
using namespace butl;

namespace build
{
  const dir_path build_dir ("build");
  const dir_path bootstrap_dir ("build/bootstrap");

  const path root_file ("build/root.build");
  const path bootstrap_file ("build/bootstrap.build");
  const path src_root_file ("build/bootstrap/src-root.build");

  bool
  is_src_root (const dir_path& d)
  {
    // @@ Can we have root without bootstrap? I don't think so.
    //
    return file_exists (d / bootstrap_file) || file_exists (d / root_file);
  }

  bool
  is_out_root (const dir_path& d)
  {
    return file_exists (d / src_root_file);
  }

  dir_path
  find_src_root (const dir_path& b)
  {
    for (dir_path d (b); !d.root () && d != home; d = d.directory ())
    {
      if (is_src_root (d))
        return d;
    }

    return dir_path ();
  }

  dir_path
  find_out_root (const dir_path& b, bool* src)
  {
    for (dir_path d (b); !d.root () && d != home; d = d.directory ())
    {
      bool s (false);
      if ((s = is_src_root (d)) || is_out_root (d)) // Order is important!
      {
        if (src != nullptr)
          *src = s;

        return d;
      }
    }

    return dir_path ();
  }

  void
  source (const path& bf, scope& root, scope& base)
  {
    tracer trace ("source");

    ifstream ifs (bf.string ());
    if (!ifs.is_open ())
      fail << "unable to open " << bf;

    level4 ([&]{trace << "sourcing " << bf;});

    ifs.exceptions (ifstream::failbit | ifstream::badbit);
    parser p;

    try
    {
      p.parse_buildfile (ifs, bf, root, base);
    }
    catch (const std::ios_base::failure&)
    {
      fail << "failed to read from " << bf;
    }
  }

  void
  source_once (const path& bf, scope& root, scope& base, scope& once)
  {
    tracer trace ("source_once");

    if (!once.buildfiles.insert (bf).second)
    {
      level4 ([&]{trace << "skipping already sourced " << bf;});
      return;
    }

    source (bf, root, base);
  }

  scope&
  create_root (const dir_path& out_root, const dir_path& src_root)
  {
    scope& rs (scopes.insert (out_root, true).first);

    // Enter built-in meta-operation and operation names. Note that
    // the order of registration should match the id constants; see
    // <operation> for details. Loading of modules (via the src
    // bootstrap; see below) can result in additional names being
    // added.
    //
    if (rs.meta_operations.empty ())
    {
      assert (rs.meta_operations.insert (perform) == perform_id);

      assert (rs.operations.insert (default_) == default_id);
      assert (rs.operations.insert (update) == update_id);
      assert (rs.operations.insert (clean) == clean_id);
    }

    // If this is already a root scope, verify that things are
    // consistent.
    //
    {
      auto v (rs.assign ("out_root"));

      if (!v)
        v = out_root;
      else
      {
        const dir_path& p (v.as<const dir_path&> ());

        if (p != out_root)
          fail << "new out_root " << out_root << " does not match "
               << "existing " << p;
      }
    }

    if (!src_root.empty ())
    {
      auto v (rs.assign ("src_root"));

      if (!v)
        v = src_root;
      else
      {
        const dir_path& p (v.as<const dir_path&> ());

        if (p != src_root)
          fail << "new src_root " << src_root << " does not match "
               << "existing " << p;
      }
    }

    return rs;
  }

  void
  bootstrap_out (scope& root)
  {
    path bf (root.path () / path ("build/bootstrap/src-root.build"));

    if (!file_exists (bf))
      return;

    //@@ TODO: if bootstrap files can source other bootstrap files
    //   (the way to express dependecies), then we need a way to
    //   prevent multiple sourcing. We handle it here but we still
    //   need something like source_once (once [scope] source).
    //
    source_once (bf, root, root);
  }

  // Extract the specified variable value from a buildfile. It is
  // expected to be the first non-comment line and not to rely on
  // any variable expansion other than those from the global scope.
  //
  static value_ptr
  extract_variable (const path& bf, const char* var)
  {
    ifstream ifs (bf.string ());
    if (!ifs.is_open ())
      fail << "unable to open " << bf;

    ifs.exceptions (ifstream::failbit | ifstream::badbit);

    try
    {
      path rbf (diag_relative (bf));

      lexer l (ifs, rbf.string ());
      token t (l.next ());
      token_type tt;

      if (t.type () != token_type::name || t.name () != var ||
          ((tt = l.next ().type ()) != token_type::equal &&
           tt != token_type::plus_equal))
        fail << "variable '" << var << "' expected as first line in " << rbf;

      parser p;
      temp_scope tmp (*global_scope);
      p.parse_variable (l, tmp, t.name (), tt);

      auto val (tmp.vars[var]);
      assert (val.defined ());
      value_ptr& vp (val);
      return move (vp); // Steal the value, the scope is going away.
    }
    catch (const std::ios_base::failure&)
    {
      fail << "failed to read from " << bf;
    }

    return nullptr;
  }

  using subprojects = map<string, dir_path>;

  // Extract the project name from bootstrap.build.
  //
  static string
  find_project_name (const dir_path& out_root,
                     const dir_path& fallback_src_root,
                     bool* src_hint = nullptr)
  {
    tracer trace ("find_project_name");

    // Load the project name. If this subdirectory is the subproject's
    // src_root, then we can get directly to that. Otherwise, we first
    // have to discover its src_root.
    //
    const dir_path* src_root;
    value_ptr src_root_vp; // Need it to live until the end.

    if (src_hint != nullptr ? *src_hint : is_src_root (out_root))
      src_root = &out_root;
    else
    {
      path f (out_root / src_root_file);

      if (!fallback_src_root.empty () && !file_exists (f))
        src_root = &fallback_src_root;
      else
      {
        src_root_vp = extract_variable (f, "src_root");
        value_proxy v (&src_root_vp, nullptr); // Read-only.
        src_root = &v.as<const dir_path&> ();
        level4 ([&]{trace << "extracted src_root " << *src_root << " for "
                          << out_root;});
      }
    }

    string name;
    {
      value_ptr vp (extract_variable (*src_root / bootstrap_file, "project"));
      value_proxy v (&vp, nullptr); // Read-only.
      name = move (v.as<string&> ());
    }

    level4 ([&]{trace << "extracted project name " << name << " for "
                      << *src_root;});
    return name;
  }

  // Scan the specified directory for any subprojects. If a subdirectory
  // is a subproject, then enter it into the map, handling the duplicates.
  // Otherwise, scan the subdirectory recursively.
  //
  static void
  find_subprojects (subprojects& sps,
                    const dir_path& d,
                    const dir_path& root,
                    bool out)
  {
    tracer trace ("find_subprojects");

    for (const dir_entry& de: dir_iterator (d))
    {
      if (de.ltype () != entry_type::directory)
        continue;

      dir_path sd (d / path_cast<dir_path> (de.path ()));

      bool src (false);
      if (!((out && is_out_root (sd)) || (src = is_src_root (sd))))
      {
        find_subprojects (sps, sd, root, out);
        continue;
      }

      // Calculate relative subdirectory for this subproject.
      //
      dir_path dir (sd.leaf (root));
      level4 ([&]{trace << "subproject " << sd << " as " << dir;});

      // Load its name. Note that here we don't use fallback src_root
      // since this function is used to scan both out_root and src_root.
      //
      string name (find_project_name (sd, dir_path (), &src));

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

        level5 ([&]{trace << "skipping duplicate";});
      }
    }
  }

  bool
  bootstrap_src (scope& root)
  {
    tracer trace ("bootstrap_src");

    bool r (false);

    const dir_path& out_root (root.path ());
    const dir_path& src_root (root.src_path ());

    path bf (src_root / path ("build/bootstrap.build"));

    if (file_exists (bf))
    {
      // We assume that bootstrap out cannot load this file explicitly. It
      // feels wrong to allow this since that makes the whole bootstrap
      // process hard to reason about. But we may try to bootstrap the
      // same root scope multiple time.
      //
      source_once (bf, root, root);
      r = true;
    }

    // See if we are a part of an amalgamation. There are two key
    // players: the outer root scope which may already be present
    // (i.e., we were loaded as part of an amalgamation) and the
    // amalgamation variable that may or may not be set by the
    // user (in bootstrap.build) or by an earlier call to this
    // function for the same scope. When set by the user, the
    // empty special value means that the project shall not be
    // amalgamated (and which we convert to NULL below). When
    // calculated, the NULL value indicates that we are not
    // amalgamated.
    //
    {
      auto rp (root.vars.assign("amalgamation")); // Set NULL by default.
      auto& val (rp.first);

      if (!val.null () && val.empty ()) // Convert empty to NULL.
        val = nullptr;

      if (scope* aroot = root.parent_scope ()->root_scope ())
      {
        const dir_path& ad (aroot->path ());
        dir_path rd (ad.relative (out_root));

        // If we already have the amalgamation variable set, verify
        // that aroot matches its value.
        //
        if (!rp.second)
        {
          if (val.null ())
          {
            fail << out_root << " cannot be amalgamated" <<
              info << "amalgamated by " << ad;
          }
          else
          {
            const dir_path& vd (val.as<const dir_path&> ());

            if (vd != rd)
            {
              fail << "inconsistent amalgamation of " << out_root <<
                info << "specified: " << vd <<
                info << "actual: " << rd << " by " << ad;
            }
          }
        }
        else
        {
          // Otherwise, use the outer root as our amalgamation.
          //
          level4 ([&]{trace << out_root << " amalgamated as " << rd;});
          val = move (rd);
        }
      }
      else if (rp.second)
      {
        // If there is no outer root and the amalgamation variable
        // hasn't been set, then we need to check if any of the
        // outer directories is a project's out_root. If so, then
        // that's our amalgamation.
        //
        const dir_path& ad (find_out_root (out_root.directory ()));

        if (!ad.empty ())
        {
          dir_path rd (ad.relative (out_root));
          level4 ([&]{trace << out_root << " amalgamated as " << rd;});
          val = move (rd);
        }
      }
    }

    // See if we have any subprojects. In a sense, this is the other
    // side/direction of the amalgamation logic above. Here, the
    // subprojects variable may or may not be set by the user (in
    // bootstrap.build) or by an earlier call to this function for
    // the same scope. When set by the user, the empty special value
    // means that there are no subproject and none should be searched
    // for (and which we convert to NULL below). Otherwise, it is a
    // list of directory[=project] pairs. The directory must be
    // relative to our out_root. If the project name is not specified,
    // then we have to figure it out. When subprojects are calculated,
    // the NULL value indicates that we found no subprojects.
    //
    {
      auto rp (root.vars.assign("subprojects")); // Set NULL by default.
      auto& val (rp.first);

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

        if (dir_exists (out_root))
          find_subprojects (sps, out_root, out_root, true);

        if (out_root != src_root)
          find_subprojects (sps, src_root, src_root, false);

        // Transform our map to list_value.
        //
        if (!sps.empty ())
        {
          list_value_ptr vp (new list_value);
          for (auto& p: sps)
          {
            vp->emplace_back (p.first);
            vp->back ().pair = '=';
            vp->emplace_back (move (p.second));
          }
          val = move (vp);
        }
      }
      else if (!val.null ())
      {
        // Convert empty to NULL.
        //
        if (val.empty ())
          val = nullptr;
        else
        {
          // Scan the value and convert it to the "canonical" form,
          // that is, a list of dir=simple pairs.
          //
          list_value& lv (val.as<list_value&> ());

          for (auto i (lv.begin ()); i != lv.end (); ++i)
          {
            bool p (i->pair != '\0');

            if (p)
            {
              // Project name.
              //
              if (!i->simple () || i->empty ())
                fail << "expected project name instead of '" << *i << "' in "
                     << "the subprojects variable";

              ++i; // Got to have the second half of the pair.
            }

            name& n (*i);

            if (n.simple ())
            {
              n.dir = dir_path (move (n.value));
              n.value.clear ();
            }

            if (!n.directory ())
              fail << "expected directory instead of '" << n << "' in the "
                   << "subprojects variable";

            // Figure out the project name if the user didn't specify one.
            //
            if (!p)
            {
              // Pass fallback src_root since this is a subproject that
              // was specified by the user so it is most likely in our
              // src.
              //
              i = lv.emplace (i, find_project_name (out_root / n.dir,
                                                    src_root / n.dir));
              i->pair = '=';
              ++i;
            }
          }
        }
      }
    }

    return r;
  }

  void
  create_bootstrap_outer (scope& root)
  {
    auto v (root.vars["amalgamation"]);

    if (!v)
      return;

    const dir_path& d (v.as<const dir_path&> ());
    dir_path out_root (root.path () / d);
    out_root.normalize ();

    // src_root is a bit more complicated. Here we have three cases:
    //
    // 1. Amalgamation's src_root is "parallel" to the sub-project's.
    // 2. Amalgamation's src_root is the same as its out_root.
    // 3. Some other pre-configured (via src-root.build) src_root.
    //
    // So we need to try all these cases in some sensible order.
    // #3 should probably be tried first since that src_root was
    // explicitly configured by the user. After that, #2 followed
    // by #1 seems reasonable.
    //
    scope& rs (create_root (out_root, dir_path ()));
    bootstrap_out (rs); // #3 happens here, if at all.

    auto val (rs.assign ("src_root"));

    if (!val)
    {
      if (is_src_root (out_root)) // #2
        val = out_root;
      else // #1
      {
        dir_path src_root (root.src_path () / d);
        src_root.normalize ();
        val = move (src_root);
      }
    }

    rs.src_path_ = &val.as<const dir_path&> ();

    bootstrap_src (rs);
    create_bootstrap_outer (rs);
  }

  scope&
  create_bootstrap_inner (scope& root, const dir_path& out_base)
  {
    if (auto v = root.vars["subprojects"])
    {
      for (const name& n: v.as<const list_value&> ())
      {
        if (n.pair != '\0')
          continue; // Skip project names.

        dir_path out_root (root.path () / n.dir);

        if (!out_base.sub (out_root))
          continue;

        // The same logic to src_root as in create_bootstrap_outer().
        //
        scope& rs (create_root (out_root, dir_path ()));
        bootstrap_out (rs);

        auto val (rs.assign ("src_root"));

        if (!val)
          val = is_src_root (out_root)
            ? out_root
            : (root.src_path () / n.dir);

        rs.src_path_ = &val.as<const dir_path&> ();

        bootstrap_src (rs);

        // See if there are more inner roots.
        //
        return create_bootstrap_inner (rs, out_base);
      }
    }

    return root;
  }

  void
  load_root_pre (scope& root)
  {
    tracer trace ("root_pre");

    // First load outer roots, if any.
    //
    if (scope* rs = root.parent_scope ()->root_scope ())
      load_root_pre (*rs);

    path bf (root.src_path () / path ("build/root.build"));

    if (file_exists (bf))
      source_once (bf, root, root);
  }

  list_value
  import (scope& ibase, const name& n, const location& l)
  {
    tracer trace ("import");

    // Split the name into the project and target.
    //
    string project;
    name target;

    if (n.dir.empty ())
    {
      if (!n.simple ())
        fail << "project name expected before imported target " << n;

      // Note that value can be foo/bar/baz; in this case probably
      // means sub-projects? Or only to a certain point, then
      // (untyped) target? Looks like I will need to scan anything
      // that looks like a directory checking if this is a subproject.
      // If not, then that's part of the target.
      //
      project = n.value;
    }
    else
    {
      //@@ This can be a path inside a sub-project. So, eventually,
      //   we should find the innermost sub-project and load the
      //   export stub from there (will probably still have to
      //   resolve root from the top-level project). For now we
      //   assume the project is always top-level.
      //
      project = *n.dir.begin ();

      target.dir = n.dir.leaf (dir_path (project));
      target.type = n.type;
      target.value = n.value;
    }

    scope& iroot (*ibase.root_scope ());

    // Figure out this project's out_root.
    //
    dir_path out_root;
    string var ("config.import." + project);

    if (auto v = iroot[var])
    {
      if (!v.belongs (*global_scope)) // A value from (some) config.build.
        out_root = v.as<const dir_path&> ();
      else
      {
        // Process the path by making it absolute and normalized. Also,
        // for usability's sake, treat a simple name that doesn't end
        // with '/' as a directory.
        //
        list_value& lv (v.as<list_value&> ());

        if (lv.size () != 1 || lv[0].empty () || !lv[0].type.empty ())
          fail (l) << "invalid " << var << " value " << lv;

        name& n (lv[0]);

        if (n.directory ())
          out_root = n.dir;
        else
          out_root = dir_path (n.value);

        if (out_root.relative ())
          out_root = work / out_root;

        out_root.normalize ();
        iroot.assign (var) = out_root;

        // Also update the command-line value. This is necessary to avoid
        // a warning issued by the config module about global/root scope
        // value mismatch.
        //
        if (n.dir != out_root)
          n = name (out_root);
      }
    }
    else
      fail (l) << "unable to find out_root for imported " << project <<
        info << "consider explicitly configuring its out_root via the "
               << var << " command line variable";

    // Bootstrap the imported root scope. This is pretty similar to
    // what we do in main() except that here we don't try to guess
    // src_root.
    //
    dir_path src_root (is_src_root (out_root) ? out_root : dir_path ());
    scope& root (create_root (out_root, src_root));

    bootstrap_out (root);

    // Check that the bootstrap process set src_root.
    //
    if (auto v = root.vars["src_root"])
    {
      const dir_path& p (v.as<const dir_path&> ());

      if (!src_root.empty () && p != src_root)
        fail << "bootstrapped src_root " << p << " does not match "
             << "discovered " << src_root;

      root.src_path_ = &p;
    }
    else
      fail (l) << "unable to determine src_root for imported " << project <<
        info << "consider configuring " << out_root;

    bootstrap_src (root);

    // Bootstrap outer roots if any. Loading will be done by
    // load_root_pre() below.
    //
    create_bootstrap_outer (root);

    // Load the imported root scope.
    //
    load_root_pre (root);

    // Create a temporary scope so that the export stub does not mess
    // up any of our variables.
    //
    temp_scope ts (ibase);

    // "Pass" the imported project's roots to the stub.
    //
    ts.assign ("out_root") = move (out_root);
    ts.assign ("src_root") = move (src_root);

    // Also pass the target being imported.
    //
    {
      auto v (ts.assign ("target"));

      if (!target.empty ()) // Otherwise leave NULL.
        v = list_value {move (target)};
    }

    // Load the export stub. Note that it is loaded in the context
    // of the importing project, not the imported one. The export
    // stub will normally switch to the imported root scope at some
    // point.
    //
    path es (root.src_path () / path ("build/export.build"));
    ifstream ifs (es.string ());
    if (!ifs.is_open ())
      fail << "unable to open " << es;

    level4 ([&]{trace << "importing " << es;});

    ifs.exceptions (ifstream::failbit | ifstream::badbit);
    parser p;

    try
    {
      p.parse_buildfile (ifs, es, iroot, ts);
    }
    catch (const std::ios_base::failure&)
    {
      fail << "failed to read from " << es;
    }

    return p.export_value ();
  }
}
