// file      : build2/file.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/file>

#include <iostream> // cin

#include <butl/filesystem> // file_exists()

#include <build2/scope>
#include <build2/context>
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

  static void
  source (const path& bf, scope& root, scope& base, bool boot)
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
    catch (const ifdstream::failure& e)
    {
      fail << "unable to read buildfile " << bf << ": " << e.what ();
    }
  }

  void
  source (const path& bf, scope& root, scope& base)
  {
    return source (bf, root, base, false);
  }

  void
  source_once (const path& bf, scope& root, scope& base, scope& once)
  {
    tracer trace ("source_once");

    if (!once.buildfiles.insert (bf).second)
    {
      l5 ([&]{trace << "skipping already sourced " << bf;});
      return;
    }

    source (bf, root, base);
  }

  scope&
  create_root (const dir_path& out_root, const dir_path& src_root)
  {
    auto i (scopes.insert (out_root, true));
    scope& rs (i->second);

    // Set out_path. src_path is set in setup_root() below.
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
      rs.meta_operations.insert (perform_id, perform);

      rs.operations.insert (default_id, default_);
      rs.operations.insert (update_id, update);
      rs.operations.insert (clean_id, clean);
    }

    // If this is already a root scope, verify that things are
    // consistent.
    //
    {
      value& v (rs.assign ("out_root"));

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
      value& v (rs.assign ("src_root"));

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
    value& v (s.assign ("src_root"));
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
    value& ov (s.assign ("out_base"));

    if (!ov)
      ov = out_base;
    else
      assert (cast<dir_path> (ov) == out_base);

    value& sv (s.assign ("src_base"));

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

  void
  bootstrap_out (scope& root)
  {
    path bf (root.out_path () / path ("build/bootstrap/src-root.build"));

    if (!file_exists (bf))
      return;

    //@@ TODO: if bootstrap files can source other bootstrap files
    //   (the way to express dependecies), then we need a way to
    //   prevent multiple sourcing. We handle it here but we still
    //   need something like source_once (once [scope] source).
    //
    source_once (bf, root, root);
  }

  // Extract the specified variable value from a buildfile. It is expected to
  // be the first non-comment line and not to rely on any variable expansion
  // other than those from the global scope or any variable overrides.
  //
  static value
  extract_variable (const path& bf, const char* name)
  {
    try
    {
      ifdstream ifs (bf);

      lexer lex (ifs, bf);
      token t (lex.next ());
      token_type tt;

      if (t.type != token_type::name || t.value != name ||
          ((tt = lex.next ().type) != token_type::assign &&
           tt != token_type::prepend &&
           tt != token_type::append))
      {
        error << "variable '" << name << "' expected as first line in " << bf;
        throw failed (); // Suppress "used uninitialized" warning.
      }

      const variable& var (var_pool.find (move (t.value)));

      parser p;
      temp_scope tmp (*global_scope);
      p.parse_variable (lex, tmp, var, tt);

      value* v (tmp.vars.find (var));
      assert (v != nullptr);
      return move (*v); // Steal the value, the scope is going away.
    }
    catch (const ifdstream::failure& e)
    {
      fail << "unable to read buildfile " << bf << ": " << e.what ();
    }

    return value (); // Never reaches.
  }

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
    value src_root_v; // Need it to live until the end.

    if (src_hint != nullptr ? *src_hint : is_src_root (out_root))
      src_root = &out_root;
    else
    {
      path f (out_root / src_root_file);

      if (!fallback_src_root.empty () && !file_exists (f))
        src_root = &fallback_src_root;
      else
      {
        src_root_v = extract_variable (f, "src_root");
        src_root = &cast<dir_path> (src_root_v);
        l5 ([&]{trace << "extracted src_root " << *src_root << " for "
                      << out_root;});
      }
    }

    string name;
    {
      value v (extract_variable (*src_root / bootstrap_file, "project"));
      name = cast<string> (move (v));
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
  find_subprojects (subprojects& sps,
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
        find_subprojects (sps, sd, root, out);
        continue;
      }

      // Calculate relative subdirectory for this subproject.
      //
      dir_path dir (sd.leaf (root));
      l5 ([&]{trace << "subproject " << sd << " as " << dir;});

      // Load its name. Note that here we don't use fallback src_root
      // since this function is used to scan both out_root and src_root.
      //
      string name (find_project_name (sd, dir_path (), &src));

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

    path bf (src_root / path ("build/bootstrap.build"));

    if (file_exists (bf))
    {
      // We assume that bootstrap out cannot load this file explicitly. It
      // feels wrong to allow this since that makes the whole bootstrap
      // process hard to reason about. But we may try to bootstrap the
      // same root scope multiple time.
      //
      if (root.buildfiles.insert (bf).second)
        source (bf, root, root, true);
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
      auto rp (root.vars.insert ("amalgamation")); // Set NULL by default.
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
      const variable& var (var_pool.find ("subprojects"));
      auto rp (root.vars.insert (var)); // Set NULL by default.
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

        if (dir_exists (out_root))
          find_subprojects (sps, out_root, out_root, true);

        if (out_root != src_root)
          find_subprojects (sps, src_root, src_root, false);

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
              n = find_project_name (out_root / d, src_root / d);

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
    auto l (root.vars["subprojects"]);
    return l.defined () && (l->null || l->type != nullptr);
  }

  void
  create_bootstrap_outer (scope& root)
  {
    auto l (root.vars["amalgamation"]);

    if (!l)
      return;

    const dir_path& d (cast<dir_path> (l));
    dir_path out_root (root.out_path () / d);
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

    if (!bootstrapped (rs))
    {
      bootstrap_out (rs); // #3 happens here, if at all.

      value& v (rs.assign ("src_root"));

      if (!v)
      {
        if (is_src_root (out_root)) // #2
          v = out_root;
        else // #1
        {
          dir_path src_root (root.src_path () / d);
          src_root.normalize ();
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
    if (auto l = root.vars["subprojects"])
    {
      for (const auto& p: cast<subprojects> (l))
      {
        dir_path out_root (root.out_path () / p.second);

        if (!out_base.sub (out_root))
          continue;

        // The same logic to src_root as in create_bootstrap_outer().
        //
        scope& rs (create_root (out_root, dir_path ()));

        if (!bootstrapped (rs))
        {
          bootstrap_out (rs);

          value& v (rs.assign ("src_root"));

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
        load_module (n, root, root, s.loc);
        assert (!s.boot);
      }
    }

    // Load root.build.
    //
    path bf (root.src_path () / path ("build/root.build"));

    if (file_exists (bf))
      source_once (bf, root, root);
  }

  names
  import (scope& ibase, name target, const location& loc)
  {
    tracer trace ("import");

    // If there is no project specified for this target, then our
    // run will be short and sweet: we simply return it as empty-
    // project-qualified and let someone else (e.g., a rule) take
    // a stab at it.
    //
    if (target.unqualified ())
    {
      target.proj = &project_name_pool.find ("");
      return names {move (target)};
    }

    // Otherwise, get the project name and convert the target to
    // unqualified.
    //
    const string& project (*target.proj);
    target.proj = nullptr;

    scope& iroot (*ibase.root_scope ());

    // Figure out this project's out_root.
    //
    dir_path out_root;

    // First search subprojects, starting with our root and then trying
    // outer roots for as long as we are inside an amalgamation.
    //
    for (scope* r (&iroot);; r = r->parent_scope ()->root_scope ())
    {
      // First check the amalgamation itself.
      //
      if (r != &iroot && cast<string> (r->vars["project"]) == project)
      {
        out_root = r->out_path ();
        break;
      }

      if (auto l = r->vars["subprojects"])
      {
        const auto& m (cast<subprojects> (l));
        auto i (m.find (project));

        if (i != m.end ())
        {
          const dir_path& d ((*i).second);
          out_root = r->out_path () / d;
          break;
        }
      }

      if (!r->vars["amalgamation"])
        break;
    }

    // Then try the config.import.* mechanism.
    //
    if (out_root.empty ())
    {
      // Note: overridable variable with path auto-completion.
      //
      const variable& var (
        var_pool.insert<abs_dir_path> ("config.import." + project, true));

      if (auto l = iroot[var])
      {
        out_root = cast<dir_path> (l);
        config::save_variable (iroot, var); // Mark as part of configuration.
      }
      else
      {
        // If we can't find the project, convert it back into qualified
        // target and return to let someone else (e.g., a rule) to take
        // a stab at it.
        //
        target.proj = &project;
        l5 ([&]{trace << "postponing " << target;});
        return names {move (target)};
      }
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
      root = &create_root (out_root, src_root);

      if (!bootstrapped (*root))
      {
        bootstrap_out (*root);

        // Check that the bootstrap process set src_root.
        //
        if (auto l = root->vars["src_root"])
        {
          const dir_path& p (cast<dir_path> (l));

          if (!src_root.empty () && p != src_root)
            fail (loc) << "bootstrapped src_root " << p << " does not match "
                       << "discovered " << src_root;
        }
        else
          fail (loc) << "unable to determine src_root for imported "
                     << project <<
            info << "consider configuring " << out_root;

        setup_root (*root);
        bootstrap_src (*root);
      }

      // Now we know this project's name as well as all its subprojects.
      //
      if (cast<string> (root->vars["project"]) == project)
        break;

      if (auto l = root->vars["subprojects"])
      {
        const auto& m (cast<subprojects> (l));
        auto i (m.find (project));

        if (i != m.end ())
        {
          const dir_path& d ((*i).second);
          out_root = root->out_path () / d;
          continue;
        }
      }

      fail (loc) << out_root << " is not out_root for " << project;
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
    ts.assign ("out_root") = move (out_root);
    ts.assign ("src_root") = move (src_root);

    // Also pass the target being imported.
    //
    {
      value& v (ts.assign ("target"));

      if (!target.empty ()) // Otherwise leave NULL.
        v = move (target);
    }

    // Load the export stub. Note that it is loaded in the context
    // of the importing project, not the imported one. The export
    // stub will normally switch to the imported root scope at some
    // point.
    //
    path es (root->src_path () / path ("build/export.build"));

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
    catch (const ifdstream::failure& e)
    {
      fail (loc) << "unable to read buildfile " << es << ": " << e.what ();
    }

    return names (); // Never reached.
  }

  target&
  import (const prerequisite_key& pk)
  {
    assert (pk.proj != nullptr);
    const string& p (*pk.proj);

    // @@ We no longer have location. This is especially bad for the
    //    empty case, i.e., where do I need to specify the project
    //    name)? Looks like the only way to do this is to keep location
    //    in name and then in prerequisite. Perhaps one day...
    //
    if (!p.empty ())
      fail << "unable to import target " << pk <<
        info << "consider explicitly specifying its project out_root via the "
           << "config.import." << p << " command line variable";
    else
      fail << "unable to import target " << pk <<
        info << "consider adding its installation location" <<
        info << "or explicitly specifying its project name";

    throw failed (); // No return.
  }
}
