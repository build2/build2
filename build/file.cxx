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
  root_pre (scope& root)
  {
    tracer trace ("root_pre");

    // First load outer roots, if any.
    //
    if (scope* rs = root.parent_scope ()->root_scope ())
      root_pre (*rs);

    path bf (root.src_path () / path ("build/root.build"));

    if (file_exists (bf))
      source_once (bf, root, root);
  }
}
