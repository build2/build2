// file      : build2/file.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/file>

#include <iostream> // cin

#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/filesystem>   // exists()
#include <build2/prerequisite>
#include <build2/diagnostics>

#include <build2/token>
#include <build2/lexer>
#include <build2/parser>

#include <build2/config/utility>

using namespace std;
using namespace butl;

namespace build2
{
  const dir_path build_dir ("build");
  const dir_path bootstrap_dir (dir_path (build_dir) /= "bootstrap");

  const path root_file (build_dir / "root.build");
  const path bootstrap_file (build_dir / "bootstrap.build");
  const path src_root_file (bootstrap_dir / "src-root.build");
  const path export_file (build_dir / "export.build");

  // While strictly speaking it belongs in, say, config/module.cxx, the static
  // initialization order strikes again. If we ever make the config module
  // loadable, then we can move it there.
  //
  const path config_file (build_dir / "config.build");

  bool
  is_src_root (const dir_path& d)
  {
    // @@ Can we have root without bootstrap? I don't think so.
    //
    return exists (d / bootstrap_file) || exists (d / root_file);
  }

  bool
  is_out_root (const dir_path& d)
  {
    return exists (d / src_root_file);
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

  static void
  source (scope& root, scope& base, const path& bf, bool boot)
  {
    tracer trace ("source");

    try
    {
      bool sin (bf.string () == "-");

      ifdstream ifs;

      if (!sin)
        ifs.open (bf);
      else
        cin.exceptions (ifdstream::failbit | ifdstream::badbit);

      istream& is (sin ? cin : ifs);

      l5 ([&]{trace << "sourcing " << bf;});

      parser p (boot);
      p.parse_buildfile (is, bf, root, base);
    }
    catch (const io_error& e)
    {
      fail << "unable to read buildfile " << bf << ": " << e;
    }
  }

  void
  source (scope& root, scope& base, const path& bf)
  {
    source (root, base, bf, false);
  }

  bool
  source_once (scope& root, scope& base, const path& bf, scope& once)
  {
    tracer trace ("source_once");

    if (!once.buildfiles.insert (bf).second)
    {
      l5 ([&]{trace << "skipping already sourced " << bf;});
      return false;
    }

    source (root, base, bf);
    return true;
  }

  scope&
  create_root (scope& l, const dir_path& out_root, const dir_path& src_root)
  {
    auto i (scopes.rw (l).insert (out_root, true));
    scope& rs (i->second);

    // Set out_path. Note that src_path is set in setup_root() below.
    //
    if (rs.out_path_ != &i->first)
    {
      assert (rs.out_path_ == nullptr);
      rs.out_path_ = &i->first;
    }

    // First time create_root() is called on this scope.
    //
    bool first (rs.meta_operations.empty ());

    // Enter built-in meta-operation and operation names. Loading of
    // modules (via the src bootstrap; see below) can result in
    // additional meta/operations being added.
    //
    if (first)
    {
      rs.meta_operations.insert (noop_id, noop);
      rs.meta_operations.insert (perform_id, perform);

      rs.operations.insert (default_id, default_);
      rs.operations.insert (update_id, update);
      rs.operations.insert (clean_id, clean);
    }

    // If this is already a root scope, verify that things are
    // consistent.
    //
    {
      value& v (rs.assign (var_out_root));

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
      value& v (rs.assign (var_src_root));

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

    return rs;
  }

  void
  setup_root (scope& s)
  {
    // The caller must have made sure src_root is set on this scope.
    //
    value& v (s.assign (var_src_root));
    assert (v);
    const dir_path& d (cast<dir_path> (v));

    if (s.src_path_ == nullptr)
      s.src_path_ = &d;
    else
      assert (s.src_path_ == &d);
  }

  scope&
  setup_base (scope_map::iterator i,
              const dir_path& out_base,
              const dir_path& src_base)
  {
    scope& s (i->second);

    // Set src/out_base variables.
    //
    value& ov (s.assign (var_out_base));

    if (!ov)
      ov = out_base;
    else
      assert (cast<dir_path> (ov) == out_base);

    value& sv (s.assign (var_src_base));

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
      s.src_path_ = &cast<dir_path> (sv);
    else
      assert (*s.src_path_ == src_base);

    return s;
  }

  pair<scope&, scope*>
  switch_scope (scope& root, const dir_path& p)
  {
    // First, enter the scope into the map and see if it is in any project. If
    // it is not, then there is nothing else to do.
    //
    auto i (scopes.rw (root).insert (p, false));
    scope& base (i->second);
    scope* rs (base.root_scope ());

    if (rs != nullptr)
    {
      // Path p can be src_base or out_base. Figure out which one it is.
      //
      dir_path out_base (p.sub (rs->out_path ()) ? p : src_out (p, *rs));

      // Create and bootstrap root scope(s) of subproject(s) that this scope
      // may belong to. If any were created, load them. Note that we need to
      // do this before figuring out src_base since we may switch the root
      // project (and src_root with it).
      //
      {
        scope* nrs (&create_bootstrap_inner (*rs, out_base));

        if (rs != nrs)
          rs = nrs;
      }

      // Switch to the new root scope.
      //
      if (rs != &root)
        load_root_pre (*rs); // Load new root(s) recursively.

      // Now we can figure out src_base and finish setting the scope.
      //
      dir_path src_base (src_out (out_base, *rs));
      setup_base (i, move (out_base), move (src_base));
    }

    return pair<scope&, scope*> (base, rs);
  }

  void
  bootstrap_out (scope& root)
  {
    path bf (root.out_path () / src_root_file);

    if (!exists (bf))
      return;

    //@@ TODO: if bootstrap files can source other bootstrap files
    //   (the way to express dependecies), then we need a way to
    //   prevent multiple sourcing. We handle it here but we still
    //   need something like source_once (once [scope] source).
    //
    source_once (root, root, bf);
  }

  // Extract the specified variable value from a buildfile. It is expected to
  // be the first non-comment line and not to rely on any variable expansion
  // other than those from the global scope or any variable overrides.
  //
  pair<value, bool>
  extract_variable (scope& s, const path& bf, const variable& var)
  {
    try
    {
      ifdstream ifs (bf);

      lexer lex (ifs, bf);
      token t (lex.next ());
      token_type tt;

      if (t.type != token_type::word || t.value != var.name ||
          ((tt = lex.next ().type) != token_type::assign &&
           tt != token_type::prepend &&
           tt != token_type::append))
      {
        return make_pair (value (), false);
      }

      parser p;
      temp_scope tmp (s.global ());
      p.parse_variable (lex, tmp, var, tt);

      value* v (tmp.vars.find_to_modify (var));
      assert (v != nullptr);

      // Steal the value, the scope is going away.
      //
      return make_pair (move (*v), true);
    }
    catch (const io_error& e)
    {
      fail << "unable to read buildfile " << bf << ": " << e << endf;
    }

    // Never reached.
  }

  // Extract the project name from bootstrap.build.
  //
  static string
  find_project_name (scope& s,
                     const dir_path& out_root,
                     const dir_path& fallback_src_root,
                     bool* src_hint = nullptr)
  {
    tracer trace ("find_project_name");

    // Load the project name. If this subdirectory is the subproject's
    // src_root, then we can get directly to that. Otherwise, we first
    // have to discover its src_root.
    //
    const dir_path* src_root;
    value src_root_v; // Need it to live until the end.

    if (src_hint != nullptr ? *src_hint : is_src_root (out_root))
      src_root = &out_root;
    else
    {
      path f (out_root / src_root_file);

      if (!fallback_src_root.empty () && !exists (f))
        src_root = &fallback_src_root;
      else
      {
        auto p (extract_variable (s, f, *var_src_root));

        if (!p.second)
          fail << "variable 'src_root' expected as first line in " << f;

        src_root_v = move (p.first);
        src_root = &cast<dir_path> (src_root_v);
        l5 ([&]{trace << "extracted src_root " << *src_root << " for "
                      << out_root;});
      }
    }

    string name;
    {
      path f (*src_root / bootstrap_file);
      auto p (extract_variable (s, f, *var_project));

      if (!p.second)
        fail << "variable '" << var_project->name << "' expected "
             << "as a first line in " << f;

      name = cast<string> (move (p.first));
    }

    l5 ([&]{trace << "extracted project name '" << name << "' for "
                  << *src_root;});
    return name;
  }

  // Scan the specified directory for any subprojects. If a subdirectory
  // is a subproject, then enter it into the map, handling the duplicates.
  // Otherwise, scan the subdirectory recursively.
  //
  static void
  find_subprojects (scope& s,
                    subprojects& sps,
                    const dir_path& d,
                    const dir_path& root,
                    bool out)
  {
    tracer trace ("find_subprojects");

    for (const dir_entry& de: dir_iterator (d))
    {
      // If this is a link, then type() will try to stat() it. And if
      // the link is dangling or points to something inaccessible, it
      // will fail.
      //
      try
      {
        if (de.type () != entry_type::directory)
          continue;
      }
      catch (const system_error& e)
      {
        continue;
      }

      dir_path sd (d / path_cast<dir_path> (de.path ()));

      bool src (false);
      if (!((out && is_out_root (sd)) || (src = is_src_root (sd))))
      {
        // We used to scan for subproject recursively but this is probably too
        // loose (think of some tests laying around). In the future we should
        // probably allow specifying something like extra/* or extra/** in
        // subprojects.
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
      string name (find_project_name (s, sd, dir_path (), &src));

      // If the name is empty, then is is an unnamed project. While the
      // 'project' variable stays empty, here we come up with a surrogate
      // name for a key. The idea is that such a key should never conflict
      // with a real project name. We ensure this by using the project's
      // sub-directory and appending trailing '/' to it.
      //
      if (name.empty ())
        name = dir.posix_string () + '/';

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

  bool
  bootstrap_src (scope& root)
  {
    tracer trace ("bootstrap_src");

    bool r (false);

    const dir_path& out_root (root.out_path ());
    const dir_path& src_root (root.src_path ());

    path bf (src_root / bootstrap_file);

    if (exists (bf))
    {
      // We assume that bootstrap out cannot load this file explicitly. It
      // feels wrong to allow this since that makes the whole bootstrap
      // process hard to reason about. But we may try to bootstrap the
      // same root scope multiple time.
      //
      if (root.buildfiles.insert (bf).second)
        source (root, root, bf, true);
      else
        l5 ([&]{trace << "skipping already sourced " << bf;});

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
      auto rp (root.vars.insert (*var_amalgamation)); // Set NULL by default.
      value& v (rp.first);

      if (v && v.empty ()) // Convert empty to NULL.
        v = nullptr;

      if (scope* aroot = root.parent_scope ()->root_scope ())
      {
        const dir_path& ad (aroot->out_path ());
        dir_path rd (ad.relative (out_root));

        // If we already have the amalgamation variable set, verify
        // that aroot matches its value.
        //
        if (!rp.second)
        {
          if (!v)
          {
            fail << out_root << " cannot be amalgamated" <<
              info << "amalgamated by " << ad;
          }
          else
          {
            const dir_path& vd (cast<dir_path> (v));

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
          l5 ([&]{trace << out_root << " amalgamated as " << rd;});
          v = move (rd);
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
          l5 ([&]{trace << out_root << " amalgamated as " << rd;});
          v = move (rd);
        }
      }
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
    {
      auto rp (root.vars.insert (*var_subprojects)); // Set NULL by default.
      value& v (rp.first);

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
          find_subprojects (root, sps, out_root, out_root, true);
        }

        if (out_root != src_root)
        {
          l5 ([&]{trace << "looking for subprojects in " << src_root;});
          find_subprojects (root, sps, src_root, src_root, false);
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
            string n;
            if (i->pair)
            {
              if (i->pair != '@')
                fail << "unexpected pair style in variable subprojects";

              try
              {
                n = convert<string> (move (*i));

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
              // Pass fallback src_root since this is a subproject that
              // was specified by the user so it is most likely in our
              // src.
              //
              n = find_project_name (root, out_root / d, src_root / d);

              // See find_subprojects() for details on unnamed projects.
              //
              if (n.empty ())
                n = d.posix_string () + '/';
            }

            sps.emplace (move (n), move (d));
          }

          // Change the value to the typed map.
          //
          v = move (sps);
        }
      }
    }

    return r;
  }

  bool
  bootstrapped (scope& root)
  {
    // Use the subprojects variable set by bootstrap_src() as an indicator.
    // It should either be NULL or typed (so we assume that the user will
    // never set it to NULL).
    //
    auto l (root.vars[var_subprojects]);
    return l.defined () && (l->null || l->type != nullptr);
  }

  void
  create_bootstrap_outer (scope& root)
  {
    auto l (root.vars[var_amalgamation]);

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
    // So we need to try all these cases in some sensible order.
    // #3 should probably be tried first since that src_root was
    // explicitly configured by the user. After that, #2 followed
    // by #1 seems reasonable.
    //
    scope& rs (create_root (root, out_root, dir_path ()));

    if (!bootstrapped (rs))
    {
      bootstrap_out (rs); // #3 happens here, if at all.

      value& v (rs.assign (var_src_root));

      if (!v)
      {
        if (is_src_root (out_root)) // #2
          v = out_root;
        else // #1
        {
          dir_path src_root (root.src_path () / d);
          src_root.normalize (); // No need to actualize (as above).
          v = move (src_root);
        }
      }

      setup_root (rs);
      bootstrap_src (rs);
    }

    create_bootstrap_outer (rs);

    // Check if we are strongly amalgamated by this outer root scope.
    //
    if (root.src_path ().sub (rs.src_path ()))
      root.strong_ = rs.strong_scope (); // Itself or some outer scope.
  }

  scope&
  create_bootstrap_inner (scope& root, const dir_path& out_base)
  {
    if (auto l = root.vars[var_subprojects])
    {
      for (const auto& p: cast<subprojects> (l))
      {
        dir_path out_root (root.out_path () / p.second);

        if (!out_base.sub (out_root))
          continue;

        // The same logic to src_root as in create_bootstrap_outer().
        //
        scope& rs (create_root (root, out_root, dir_path ()));

        if (!bootstrapped (rs))
        {
          bootstrap_out (rs);

          value& v (rs.assign (var_src_root));

          if (!v)
            v = is_src_root (out_root)
              ? out_root
              : (root.src_path () / p.second);

          setup_root (rs);
          bootstrap_src (rs);
        }

        // Check if we strongly amalgamated this inner root scope.
        //
        if (rs.src_path ().sub (root.src_path ()))
          rs.strong_ = root.strong_scope (); // Itself or some outer scope.

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

    // Finish off loading bootstrapped modules.
    //
    for (auto& p: root.modules)
    {
      const string& n (p.first);
      module_state& s (p.second);

      if (s.boot)
      {
        load_module (root, root, n, s.loc);
        assert (!s.boot);
      }
    }

    // Load root.build.
    //
    path bf (root.src_path () / root_file);

    if (exists (bf))
      source_once (root, root, bf);
  }

  names
  import (scope& ibase, name target, const location& loc)
  {
    tracer trace ("import");

    l5 ([&]{trace << target << " from " << ibase.out_path ();});

    // If there is no project specified for this target, then our run will be
    // short and sweet: we simply return it as empty-project-qualified and
    // let someone else (e.g., a rule) take a stab at it.
    //
    if (target.unqualified ())
    {
      target.proj = string ();
      return names {move (target)};
    }

    // Otherwise, get the project name and convert the target to unqualified.
    //
    string proj (move (*target.proj));
    target.proj = nullopt;

    scope& iroot (*ibase.root_scope ());

    // Figure out this project's out_root.
    //
    dir_path out_root;

    // First try the config.import.* mechanism. The idea is that if the user
    // explicitly told us the project's location, then we should prefer that
    // over anything that we may discover. In particular, we will prefer it
    // over any bundled subprojects.
    //
    auto& vp (var_pool.rw (iroot));

    for (;;) // Break-out loop.
    {
      string n ("config.import." + proj);

      // config.import.<proj>
      //
      {
        // Note: pattern-typed in context.cxx:reset() as an overridable
        // variable of type abs_dir_path (path auto-completion).
        //
        const variable& var (vp.insert (n));

        if (auto l = iroot[var])
        {
          out_root = cast<dir_path> (l);      // Normalized and actualized.
          config::save_variable (iroot, var); // Mark as part of config.

          // Empty config.import.* value means don't look in subprojects or
          // amalgamations and go straight to the rule-specific import (e.g.,
          // to use system-installed).
          //
          if (out_root.empty ())
          {
            target.proj = move (proj);
            l5 ([&]{trace << "skipping " << target;});
            return names {move (target)};
          }

          break;
        }
      }

      // config.import.<proj>.<name>.<type>
      // config.import.<proj>.<name>
      //
      // For example: config.import.build2.b.exe=/opt/build2/bin/b
      //
      if (!target.value.empty ())
      {
        auto lookup = [&iroot, &vp, &loc] (string name) -> path
        {
          // Note: pattern-typed in context.cxx:reset() as an overridable
          // variable of type path.
          //
          const variable& var (vp.insert (move (name)));

          path r;
          if (auto l = iroot[var])
          {
            r = cast<path> (l);

            if (r.empty ())
              fail (loc) << "empty path in " << var.name;

            config::save_variable (iroot, var);
          }

          return r;
        };

        // First try .<name>.<type>, then just .<name>.
        //
        path p;
        if (target.typed ())
          p = lookup (n + '.' + target.value + '.' + target.type);

        if (p.empty ())
          p = lookup (n + '.' + target.value);

        if (!p.empty ())
        {
          // If the path is relative, then keep it project-qualified assuming
          // import phase 2 knows what to do with it. Think:
          //
          // config.import.build2.b=b-boot
          //
          if (p.relative ())
            target.proj = move (proj);

          target.dir = p.directory ();
          target.value = p.leaf ().string ();

          return names {move (target)};
        }
      }

      // Otherwise search subprojects, starting with our root and then trying
      // outer roots for as long as we are inside an amalgamation.
      //
      for (scope* r (&iroot);; r = r->parent_scope ()->root_scope ())
      {
        l5 ([&]{trace << "looking in " << r->out_path ();});

        // First check the amalgamation itself.
        //
        if (r != &iroot && cast<string> (r->vars[var_project]) == proj)
        {
          out_root = r->out_path ();
          break;
        }

        if (auto l = r->vars[var_subprojects])
        {
          const auto& m (cast<subprojects> (l));
          auto i (m.find (proj));

          if (i != m.end ())
          {
            const dir_path& d ((*i).second);
            out_root = r->out_path () / d;
            break;
          }
        }

        if (!r->vars[var_amalgamation])
          break;
      }

      break;
    }

    // If we couldn't find the project, convert it back into qualified target
    // and return to let someone else (e.g., a rule) take a stab at it.
    //
    if (out_root.empty ())
    {
      target.proj = move (proj);
      l5 ([&]{trace << "postponing " << target;});
      return names {move (target)};
    }

    // Bootstrap the imported root scope. This is pretty similar to what we do
    // in main() except that here we don't try to guess src_root.
    //
    // The user can also specify the out_root of the amalgamation that contains
    // our project. For now we only consider top-level sub-projects.
    //
    dir_path src_root;
    scope* root;

    for (;;)
    {
      src_root = is_src_root (out_root) ? out_root : dir_path ();
      root = &create_root (iroot, out_root, src_root);

      if (!bootstrapped (*root))
      {
        bootstrap_out (*root);

        // Check that the bootstrap process set src_root.
        //
        if (auto l = root->vars[*var_src_root])
        {
          const dir_path& p (cast<dir_path> (l));

          if (!src_root.empty () && p != src_root)
            fail (loc) << "bootstrapped src_root " << p << " does not match "
                       << "discovered " << src_root;
        }
        else
          fail (loc) << "unable to determine src_root for imported " << proj <<
            info << "consider configuring " << out_root;

        setup_root (*root);
        bootstrap_src (*root);
      }
      else if (src_root.empty ())
        src_root = root->src_path ();

      // Now we know this project's name as well as all its subprojects.
      //
      if (cast<string> (root->vars[var_project]) == proj)
        break;

      if (auto l = root->vars[var_subprojects])
      {
        const auto& m (cast<subprojects> (l));
        auto i (m.find (proj));

        if (i != m.end ())
        {
          const dir_path& d ((*i).second);
          out_root = root->out_path () / d;
          continue;
        }
      }

      fail (loc) << out_root << " is not out_root for " << proj;
    }

    // Bootstrap outer roots if any. Loading will be done by
    // load_root_pre() below.
    //
    create_bootstrap_outer (*root);

    // Load the imported root scope.
    //
    load_root_pre (*root);

    // Create a temporary scope so that the export stub does not mess
    // up any of our variables.
    //
    temp_scope ts (ibase);

    // "Pass" the imported project's roots to the stub.
    //
    ts.assign (var_out_root) = move (out_root);
    ts.assign (var_src_root) = move (src_root);

    // Also pass the target being imported in the import.target variable.
    //
    {
      value& v (ts.assign (var_import_target));

      if (!target.empty ()) // Otherwise leave NULL.
        v = move (target);
    }

    // Load the export stub. Note that it is loaded in the context
    // of the importing project, not the imported one. The export
    // stub will normally switch to the imported root scope at some
    // point.
    //
    path es (root->src_path () / export_file);

    try
    {
      ifdstream ifs (es);

      l5 ([&]{trace << "importing " << es;});

      // @@ Should we verify these are all unqualified names? Or maybe
      // there is a use-case for the export stub to return a qualified
      // name?
      //
      parser p;
      return p.parse_export_stub (ifs, es, iroot, ts);
    }
    catch (const io_error& e)
    {
      fail (loc) << "unable to read buildfile " << es << ": " << e;
    }

    return names (); // Never reached.
  }

  const target*
  import (const prerequisite_key& pk, bool existing)
  {
    tracer trace ("import");

    assert (pk.proj);
    const string& p (*pk.proj);

    // Target type-specific search.
    //
    const target_key& tk (pk.tk);
    const target_type& tt (*tk.type);

    // Try to find the executable in PATH (or CWD if relative).
    //
    if (tt.is_a<exe> ())
    {
      path n (*tk.dir);
      n /= *tk.name;
      if (tk.ext)
      {
        n += '.';
        n += *tk.ext;
      }

      process_path pp (process::try_path_search (n, true));

      if (!pp.empty ())
      {
        path& p (pp.effect);
        assert (!p.empty ()); // We searched for a simple name.

        const exe* t (
          !existing
          ? &targets.insert<exe> (tt,
                                  p.directory (),
                                  dir_path (),    // No out (out of project).
                                  p.leaf ().base ().string (),
                                  p.extension (), // Always specified.
                                  trace)
          : targets.find<exe> (tt,
                               p.directory (),
                               dir_path (),
                               p.leaf ().base ().string (),
                               p.extension (),
                               trace));

        if (t != nullptr)
        {
          if (!existing)
            t->path (move (p));
          else
            assert (t->path () == p);

          return t;
        }
      }
    }

    if (existing)
      return nullptr;

    // @@ We no longer have location. This is especially bad for the
    //    empty case, i.e., where do I need to specify the project
    //    name)? Looks like the only way to do this is to keep location
    //    in name and then in prerequisite. Perhaps one day...
    //
    diag_record dr;
    dr << fail << "unable to import target " << pk;

    if (p.empty ())
      dr << info << "consider adding its installation location" <<
        info << "or explicitly specify its project name";
    else
      dr << info << "use config.import." << p << " command line variable to "
         << "specifying its project out_root";

    dr << endf;
  }
}
