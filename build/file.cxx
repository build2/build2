// file      : build/file.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/file>

#include <fstream>

#include <build/scope>
#include <build/parser>
#include <build/filesystem>
#include <build/diagnostics>

using namespace std;

namespace build
{
  bool
  is_src_root (const path& d)
  {
    return file_exists (d / path ("build/bootstrap.build")) ||
      file_exists (d / path ("build/root.build"));
  }

  bool
  is_out_root (const path& d)
  {
    return file_exists (d / path ("build/bootstrap/src-root.build"));
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
  create_root (const path& out_root, const path& src_root)
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
      auto v (rs.variables["out_root"]);

      if (!v)
        v = out_root;
      else
      {
        const path& p (v.as<const path&> ());

        if (p != out_root)
          fail << "new out_root " << out_root << " does not match "
               << "existing " << p;
      }
    }

    if (!src_root.empty ())
    {
      auto v (rs.variables["src_root"]);

      if (!v)
        v = src_root;
      else
      {
        const path& p (v.as<const path&> ());

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

  bool
  bootstrap_src (scope& root)
  {
    tracer trace ("bootstrap_src");

    path bf (root.src_path () / path ("build/bootstrap.build"));

    if (!file_exists (bf))
      return false;

    // We assume that bootstrap out cannot load this file explicitly. It
    // feels wrong to allow this since that makes the whole bootstrap
    // process hard to reason about. But we may try to bootstrap the
    // same root scope multiple time.
    //
    source_once (bf, root, root);
    return true;
  }

  void
  create_bootstrap_outer (scope& root)
  {
    auto v (root.ro_variables ()["amalgamation"]);

    if (!v)
      return;

    const path& d (v.as<const path&> ());
    path out_root (root.path () / d);
    path src_root (root.src_path () / d);
    out_root.normalize ();
    src_root.normalize ();

    scope& rs (create_root (out_root, src_root));

    bootstrap_out (rs);

    // Check if the bootstrap process changed src_root.
    //
    const path& p (rs.variables["src_root"].as<const path&> ());

    if (src_root != p)
      fail << "bootstrapped src_root " << p << " does not match "
           << "amalgamated " << src_root;

    rs.src_path_ = &p;

    bootstrap_src (rs);
    create_bootstrap_outer (rs);
  }

  scope&
  create_bootstrap_inner (scope& root, const path& out_base)
  {
    if (auto v = root.ro_variables ()["subprojects"])
    {
      for (const name& n: v.as<const list_value&> ())
      {
        // Should be a list of directories.
        //
        if (!n.type.empty () || !n.value.empty () || n.dir.empty ())
          fail << "expected directory in subprojects variable "
               << "instead of " << n;

        path out_root (root.path () / n.dir);

        if (!out_base.sub (out_root))
          continue;

        path src_root (root.src_path () / n.dir);
        scope& rs (create_root (out_root, src_root));

        bootstrap_out (rs);

        // Check if the bootstrap process changed src_root.
        //
        const path& p (rs.variables["src_root"].as<const path&> ());

        if (src_root != p)
          fail << "bootstrapped src_root " << p << " does not match "
               << "subproject " << src_root;

        rs.src_path_ = &p;

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

  void
  import (scope& ibase, const name& n, const location& l)
  {
    scope& iroot (*ibase.root_scope ());

    // Figure out this project's out_root.
    //
    const variable& var (variable_pool.find ("config." + n.value));
    auto val (iroot[var]);

    if (val)
    {
      if (val.scope == global_scope)
        iroot.variables[var] = val; // Copy into root scope.
    }
    else
      fail (l) << "unable to find out_root for imported " << n <<
        info << "consider explicitly configuring its out_root via the "
               << var.name << " command line variable";

    const path& out_root (val.as<const path&> ());

    // Bootstrap the imported root scope. This is pretty similar to
    // what we do in main() except that here we don't try to guess
    // src_root.
    //
    path src_root (is_src_root (out_root) ? out_root : path ());
    scope& root (create_root (out_root, src_root));

    bootstrap_out (root);

    // Check that the bootstrap process set src_root.
    //
    if (auto v = root.ro_variables ()["src_root"])
    {
      const path& p (v.as<const path&> ());

      if (!src_root.empty () && p != src_root)
        fail << "bootstrapped src_root " << p << " does not match "
             << "discovered " << src_root;

      root.src_path_ = &p;
    }
    else
      fail (l) << "unable to determine src_root for imported " << n <<
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
    ts.variables["out_root"] = out_root;
    ts.variables["src_root"] = src_root;

    // Load the export stub. Note that it is loaded in the context
    // of the importing project, not the imported one. The export
    // stub will normally switch to the imported root scope at some
    // point.
    //
    source (root.src_path () / path ("build/export.build"), iroot, ts);
  }
}
