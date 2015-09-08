// file      : build/file.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/file>

#include <fstream>
#include <utility> // move()

#include <butl/filesystem>

#include <build/scope>
#include <build/context>
#include <build/parser>
#include <build/prerequisite>
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

    level5 ([&]{trace << "sourcing " << bf;});

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
      level5 ([&]{trace << "skipping already sourced " << bf;});
      return;
    }

    source (bf, root, base);
  }

  scope&
  create_root (const dir_path& out_root, const dir_path& src_root)
  {
    auto i (scopes.insert (out_root, nullptr, true, true));
    scope& rs (*i->second);

    // Set out_path. src_path is set in setup_root() below.
    //
    if (rs.out_path_ != &i->first)
    {
      assert (rs.out_path_ == nullptr);
      rs.out_path_ = &i->first;
    }

    // Enter built-in meta-operation and operation names. Loading of
    // modules (via the src bootstrap; see below) can result in
    // additional meta/operations being added.
    //
    if (rs.meta_operations.empty ())
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
        const dir_path& p (as<dir_path> (v));

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
        const dir_path& p (as<dir_path> (v));

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
    value& v (s.assign ("src_root"));
    assert (v);

    // Register and set src_path.
    //
    if (s.src_path_ == nullptr)
      s.src_path_ = &scopes.insert (as<dir_path> (v), &s, false, true)->first;
  }

  scope&
  setup_base (scope_map::iterator i,
              const dir_path& out_base,
              const dir_path& src_base)
  {
    scope& s (*i->second);

    // Set src/out_path. The key (i->first) can be either out_base
    // or src_base.
    //
    if (s.out_path_ == nullptr)
    {
      s.out_path_ =
        i->first == out_base
        ? &i->first
        : &scopes.insert (out_base, &s, true, false)->first;
    }

    if (s.src_path_ == nullptr)
    {
      s.src_path_ =
        i->first == src_base
        ? &i->first
        : &scopes.insert (src_base, &s, false, false)->first;
    }

    // Set src/out_base variables.
    //
    {
      value& v (s.assign ("out_base"));

      if (!v)
        v = out_base;
      else
        assert (as<dir_path> (v) == out_base);
    }

    {
      value& v (s.assign ("src_base"));

      if (!v)
        v = src_base;
      else
        assert (as<dir_path> (v) == src_base);
    }

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

  // Extract the specified variable value from a buildfile. It is
  // expected to be the first non-comment line and not to rely on
  // any variable expansion other than those from the global scope.
  //
  static value
  extract_variable (const path& bf, const char* var)
  {
    ifstream ifs (bf.string ());
    if (!ifs.is_open ())
      fail << "unable to open " << bf;

    ifs.exceptions (ifstream::failbit | ifstream::badbit);

    try
    {
      path rbf (diag_relative (bf));

      lexer lex (ifs, rbf.string ());
      token t (lex.next ());
      token_type tt;

      if (t.type () != token_type::name || t.name () != var ||
          ((tt = lex.next ().type ()) != token_type::equal &&
           tt != token_type::plus_equal))
      {
        error << "variable '" << var << "' expected as first line in " << rbf;
        throw failed (); // Suppress "used uninitialized" warning.
      }

      parser p;
      temp_scope tmp (*global_scope);
      p.parse_variable (lex, tmp, t.name (), tt);

      auto l (tmp.vars[var]);
      assert (l.defined ());
      value& v (*l);
      return move (v); // Steal the value, the scope is going away.
    }
    catch (const std::ios_base::failure&)
    {
      fail << "failed to read from " << bf;
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
        src_root = &as<dir_path> (src_root_v);
        level5 ([&]{trace << "extracted src_root " << *src_root << " for "
                          << out_root;});
      }
    }

    string name;
    {
      value v (extract_variable (*src_root / bootstrap_file, "project"));
      name = move (as<string> (v));
    }

    level5 ([&]{trace << "extracted project name " << name << " for "
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
      if (de.type () != entry_type::directory)
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
      level5 ([&]{trace << "subproject " << sd << " as " << dir;});

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

        level6 ([&]{trace << "skipping duplicate";});
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
      auto rp (root.vars.assign ("amalgamation")); // Set NULL by default.
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
            const dir_path& vd (as<dir_path> (v));

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
          level5 ([&]{trace << out_root << " amalgamated as " << rd;});
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
          level5 ([&]{trace << out_root << " amalgamated as " << rd;});
          v = move (rd);
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
      const variable& var (variable_pool.find ("subprojects"));
      auto rp (root.vars.assign(var)); // Set NULL by default.
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
          // Pre-scan the value and convert it to the "canonical" form,
          // that is, a list of simple=dir pairs.
          //
          for (auto i (v.data_.begin ()); i != v.data_.end (); ++i)
          {
            bool p (i->pair != '\0');

            if (p)
            {
              // Project name.
              //
              if (!assign<string> (*i) || as<string> (*i).empty ())
                fail << "expected project name instead of '" << *i << "' in "
                     << "the subprojects variable";

              ++i; // Got to have the second half of the pair.
            }

            if (!assign<dir_path> (*i))
              fail << "expected directory instead of '" << *i << "' in the "
                   << "subprojects variable";

            auto& d (as<dir_path> (*i));

            // Figure out the project name if the user didn't specify one.
            //
            if (!p)
            {
              // Pass fallback src_root since this is a subproject that
              // was specified by the user so it is most likely in our
              // src.
              //
              i = v.data_.emplace (
                i,
                find_project_name (out_root / d, src_root / d));

              i->pair = '=';
              ++i;
            }
          }

          // Make it of the map type.
          //
          assign<subprojects> (v, var);
        }
      }
    }

    return r;
  }

  void
  create_bootstrap_outer (scope& root)
  {
    auto l (root.vars["amalgamation"]);

    if (!l)
      return;

    const dir_path& d (as<dir_path> (*l));
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
      for (const name& n: *l)
      {
        if (n.pair != '\0')
          continue; // Skip project names.

        dir_path out_root (root.out_path () / n.dir);

        if (!out_base.sub (out_root))
          continue;

        // The same logic to src_root as in create_bootstrap_outer().
        //
        scope& rs (create_root (out_root, dir_path ()));
        bootstrap_out (rs);

        value& v (rs.assign ("src_root"));

        if (!v)
          v = is_src_root (out_root)
            ? out_root
            : (root.src_path () / n.dir);

        setup_root (rs);

        bootstrap_src (rs);

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
    dir_path fallback_src_root; // We have seen this already, havent' we ?

    // First search subprojects, starting with our root and then trying
    // outer roots for as long as we are inside an amalgamation.
    //
    for (scope* r (&iroot);; r = r->parent_scope ()->root_scope ())
    {
      if (auto l = r->vars["subprojects"])
      {
        const auto& m (as<subprojects> (*l));
        auto i (m.find (project));

        if (i != m.end ())
        {
          const dir_path& d ((*i).second);
          out_root = r->out_path () / d;
          fallback_src_root = r->src_path () / d;
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
      const variable& var (
        variable_pool.find ("config.import." + project,
                            dir_path_type));

      if (auto l = iroot[var])
      {
        out_root = as<dir_path> (*l);

        if (l.belongs (*global_scope)) // A value from command line.
        {
          // Process the path by making it absolute and normalized.
          //
          if (out_root.relative ())
            out_root = work / out_root;

          out_root.normalize ();

          // Set on our root scope (part of our configuration).
          //
          iroot.assign (var) = out_root;

          // Also update the command-line value. This is necessary to avoid
          // a warning issued by the config module about global/root scope
          // value mismatch. Not very clean.
          //
          dir_path& d (as<dir_path> (const_cast<value&> (*l)));
          if (d != out_root)
            d = out_root;
        }
      }
      else
      {
        // If we can't find the project, convert it back into qualified
        // target and return to let someone else (e.g., a rule) to take
        // a stab at it.
        //
        target.proj = &project;
        level5 ([&]{trace << "postponing " << target;});
        return names {move (target)};
      }
    }

    // Bootstrap the imported root scope. This is pretty similar to
    // what we do in main() except that here we don't try to guess
    // src_root.
    //
    dir_path src_root (is_src_root (out_root) ? out_root : dir_path ());
    scope& root (create_root (out_root, src_root));

    bootstrap_out (root);

    // Check that the bootstrap process set src_root.
    //
    if (auto l = root.vars["src_root"])
    {
      const dir_path& p (as<dir_path> (*l));

      if (!src_root.empty () && p != src_root)
        fail (loc) << "bootstrapped src_root " << p << " does not match "
                   << "discovered " << src_root;
    }
    // Otherwise, use fallback if available.
    //
    else if (!fallback_src_root.empty ())
    {
      value& v (root.assign ("src_root"));
      v = move (fallback_src_root);
    }
    else
      fail (loc) << "unable to determine src_root for imported " << project <<
        info << "consider configuring " << out_root;

    setup_root (root);

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
      value& v (ts.assign ("target"));

      if (!target.empty ()) // Otherwise leave NULL.
        v = move (target);
    }

    // Load the export stub. Note that it is loaded in the context
    // of the importing project, not the imported one. The export
    // stub will normally switch to the imported root scope at some
    // point.
    //
    path es (root.src_path () / path ("build/export.build"));
    ifstream ifs (es.string ());
    if (!ifs.is_open ())
      fail (loc) << "unable to open " << es;

    level5 ([&]{trace << "importing " << es;});

    ifs.exceptions (ifstream::failbit | ifstream::badbit);
    parser p;

    try
    {
      // @@ Should we verify these are all unqualified names? Or maybe
      // there is a use-case for the export stub to return a qualified
      // name?
      //
      return p.parse_export_stub (ifs, es, iroot, ts);
    }
    catch (const std::ios_base::failure&)
    {
      fail (loc) << "failed to read from " << es;
    }

    return names (); // Never reached.
  }

  target&
  import (const prerequisite_key& pk)
  {
    assert (*pk.proj != nullptr);
    const string& p (**pk.proj);

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
