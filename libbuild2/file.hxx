// file      : libbuild2/file.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_FILE_HXX
#define LIBBUILD2_FILE_HXX

#include <map>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx> // list_value

#include <libbuild2/export.hxx>

namespace build2
{
  using subprojects = std::map<project_name, dir_path>;

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const subprojects&); // Print as name@dir sequence.

  LIBBUILD2_SYMEXPORT extern const dir_path std_build_dir;  // build/

  // build/root.build
  //
  LIBBUILD2_SYMEXPORT extern const path std_root_file;

  // build/bootstrap.build
  //
  LIBBUILD2_SYMEXPORT extern const path std_bootstrap_file;

  LIBBUILD2_SYMEXPORT extern const path std_buildfile_file; // buildfile
  LIBBUILD2_SYMEXPORT extern const path alt_buildfile_file; // build2file

  // If the altn argument value is present, then it indicates whether we are
  // using the standard or the alternative build file/directory naming.
  //
  // The overall plan is to run various "file exists" tests using the standard
  // and the alternative names. The first test that succeeds determines the
  // naming scheme (by setting altn) and from then on all the remaining tests
  // only look for things in this scheme.
  //
  LIBBUILD2_SYMEXPORT bool
  is_src_root (const dir_path&, optional<bool>& altn);

  LIBBUILD2_SYMEXPORT bool
  is_out_root (const dir_path&, optional<bool>& altn);

  // Given an src_base directory, look for a project's src_root based on the
  // presence of known special files. Return empty path if not found. Note
  // that if the input is normalized/actualized, then the output will be as
  // well.
  //
  LIBBUILD2_SYMEXPORT dir_path
  find_src_root (const dir_path&, optional<bool>& altn);

  // The same as above but for project's out. Note that we also check whether
  // a directory happens to be src_root, in case this is an in-tree build with
  // the result returned as the second half of the pair. Note also that if the
  // input is normalized/actualized, then the output will be as well.
  //
  LIBBUILD2_SYMEXPORT pair<dir_path, bool>
  find_out_root (const dir_path&, optional<bool>& altn);

  // If buildfile is '-', then read from STDIN.
  //
  LIBBUILD2_SYMEXPORT void
  source (scope& root, scope& base, const path&);

  // As above but first check if this buildfile has already been sourced for
  // the base scope. Return false if the file has already been sourced.
  //
  bool
  source_once (scope& root, scope& base, const path&);

  // As above but checks against the specified scope rather than base.
  //
  LIBBUILD2_SYMEXPORT bool
  source_once (scope& root, scope& base, const path&, scope& once);

  // Create project's root scope. Only set the src_root variable if the passed
  // src_root value is not empty. The scope argument is only used for context
  // and as a proof of lock.
  //
  LIBBUILD2_SYMEXPORT scope_map::iterator
  create_root (scope&, const dir_path& out_root, const dir_path& src_root);

  // Setup root scope. Note that it assumes the src_root variable has already
  // been set.
  //
  LIBBUILD2_SYMEXPORT void
  setup_root (scope&, bool forwarded);

  // Setup the base scope (set *_base variables, etc).
  //
  LIBBUILD2_SYMEXPORT scope&
  setup_base (scope_map::iterator,
              const dir_path& out_base,
              const dir_path& src_base);

  // Return a scope for the specified directory (first). Note that switching
  // to this scope might also involve switch to a new root scope (second) if
  // the new scope is in another project. If the new scope is not in any
  // project, then NULL is returned in second.
  //
  LIBBUILD2_SYMEXPORT pair<scope&, scope*>
  switch_scope (scope& root, const dir_path&);

  // Bootstrap and optionally load an ad hoc (sub)project (i.e., the kind that
  // is not discovered and loaded automatically by bootstrap/load functions
  // above).
  //
  // Note that we expect the outer project (if any) to be bootstrapped and
  // loaded and currently we do not add the newly loaded subproject to the
  // outer project's subprojects map.
  //
  // The scope argument is only used as proof of lock.
  //
  LIBBUILD2_SYMEXPORT scope&
  load_project (scope&,
                const dir_path& out_root,
                const dir_path& src_root,
                bool forwarded,
                bool load = true);

  // Bootstrap the project's forward. Return the forwarded-to out_root or
  // src_root if there is no forward. See is_{src,out}_root() for the altn
  // argument semantics.
  //
  LIBBUILD2_SYMEXPORT dir_path
  bootstrap_fwd (context&, const dir_path& src_root, optional<bool>& altn);

  // Bootstrap the project's root scope, the out part.
  //
  LIBBUILD2_SYMEXPORT void
  bootstrap_out (scope& root, optional<bool>& altn);

  // Bootstrap the project's root scope, the src part. Return true if we
  // loaded anything (which confirms the src_root is not bogus).
  //
  LIBBUILD2_SYMEXPORT bool
  bootstrap_src (scope& root, optional<bool>& altn);

  // Return true if this scope has already been bootstrapped, that is, the
  // following calls have already been made:
  //
  //   bootstrap_out()
  //   setup_root()
  //   bootstrap_src()
  //
  LIBBUILD2_SYMEXPORT bool
  bootstrapped (scope& root);

  // Execute pre/post-bootstrap hooks. Similar to bootstrap_out/src(), should
  // only be called once per project bootstrap.
  //
  LIBBUILD2_SYMEXPORT void
  bootstrap_pre (scope& root, optional<bool>& altn);

  LIBBUILD2_SYMEXPORT void
  bootstrap_post (scope& root);

  // Create and bootstrap outer root scopes, if any. Loading is done by
  // load_root().
  //
  LIBBUILD2_SYMEXPORT void
  create_bootstrap_outer (scope& root);

  // Create and bootstrap inner root scopes, if any, recursively.
  //
  // If out_base is not empty, then only bootstrap scope between root and base
  // returning the innermost created root scope or root if none were created.
  //
  // Note that loading is done by load_root().
  //
  LIBBUILD2_SYMEXPORT scope&
  create_bootstrap_inner (scope& root, const dir_path& out_base = dir_path ());

  // Load project's root.build (and root pre/post hooks) unless already
  // loaded. Also make sure all outer root scopes are loaded prior to loading
  // this root scope.
  //
  LIBBUILD2_SYMEXPORT void
  load_root (scope& root);

  // Extract the specified variable value from a buildfile. It is expected to
  // be the first non-comment line and not to rely on any variable expansion
  // other than those from the global scope or any variable overrides. Return
  // an indication of whether the variable was found.
  //
  LIBBUILD2_SYMEXPORT pair<value, bool>
  extract_variable (context&, const path&, const variable&);

  // Import has two phases: the first is triggered by the import directive in
  // the buildfile. It will try to find and load the project. Failed that, it
  // will return the project-qualified name of the target which will be used
  // to create a project-qualified prerequisite. This gives the rule that will
  // be searching this prerequisite a chance to do some target-type specific
  // search. For example, a C++ link rule can search for lib{} prerequisites
  // in the C++ compiler default library search paths (so that we end up with
  // functionality identical to -lfoo). If, however, the rule didn't do any of
  // that (or failed to find anything usable), it calls the standard
  // prerequisite search() function which sees this is a project-qualified
  // prerequisite and goes straight to the second phase of import. Here,
  // currently, we simply fail but in the future this will be the place where
  // we can call custom "last resort" import hooks. For example, we can hook a
  // package manager that will say, "Hey, dude, I see you are trying to import
  // foo and I see there is a package foo available in repository bar. Wanna,
  // like, download and use it or something?"
  //
  // Note also that we return names rather than a single name: while normally
  // it will be a single target name, it can be an out-qualified pair (if
  // someone wants to return a source target) but it can also be a non-target
  // since we don't restrict what users can import/export.
  //
  LIBBUILD2_SYMEXPORT names
  import (scope& base, name, const location&);

  LIBBUILD2_SYMEXPORT pair<name, dir_path>
  import_search (scope& base, name, const location&, bool subproj = true);

  LIBBUILD2_SYMEXPORT pair<names, const scope&>
  import_load (context&, pair<name, dir_path>, const location&);

  const target&
  import (context&, const prerequisite_key&);

  // As above but only imports as an already existing target. Unlike the above
  // version, this one can be called during the execute phase.
  //
  // Note: similar to search_existing().
  //
  const target*
  import_existing (context&, const prerequisite_key&);
}

#include <libbuild2/file.ixx>

#endif // LIBBUILD2_FILE_HXX
