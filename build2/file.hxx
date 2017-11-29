// file      : build2/file.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_FILE_HXX
#define BUILD2_FILE_HXX

#include <map>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/scope.hxx>
#include <build2/variable.hxx> // list_value

namespace build2
{
  class target;
  class location;
  class prerequisite_key;

  using subprojects = std::map<string, dir_path>;

  extern const dir_path build_dir;     // build
  extern const dir_path bootstrap_dir; // build/bootstrap

  extern const path root_file;         // build/root.build
  extern const path bootstrap_file;    // build/bootstrap.build
  extern const path src_root_file;     // build/bootstrap/src-root.build
  extern const path export_file;       // build/export.build
  extern const path config_file;       // build/config.build

  bool
  is_src_root (const dir_path&);

  bool
  is_out_root (const dir_path&);

  // Given an src_base directory, look for a project's src_root based on the
  // presence of known special files. Return empty path if not found. Note
  // that if the input is normalized/actualized, then the output will be as
  // well.
  //
  dir_path
  find_src_root (const dir_path&);

  // The same as above but for project's out. Note that we also check whether
  // a directory happens to be src_root, in case this is an in-tree build with
  // the result returned as the second half of the pair. Note also that if the
  // input is normalized/actualized, then the output will be as well.
  //
  pair<dir_path, bool>
  find_out_root (const dir_path&);

  // If buildfile is '-', then read from STDIN.
  //
  void
  source (scope& root, scope& base, const path&);

  // As above but first check if this buildfile has already been sourced for
  // the base scope. Return false if the file has already been sourced.
  //
  bool
  source_once (scope& root, scope& base, const path&);

  // As above but checks against the specified scope rather than base.
  //
  bool
  source_once (scope& root, scope& base, const path&, scope& once);

  // Create project's root scope. Only set the src_root variable if the
  // passed src_root value is not empty. The scope argument is only used
  // as proof of lock.
  //
  scope_map::iterator
  create_root (scope&, const dir_path& out_root, const dir_path& src_root);

  // Setup root scope. Note that it assume the src_root variable
  // has already been set.
  //
  void
  setup_root (scope&);

  // Setup the base scope (set *_base variables, etc).
  //
  scope&
  setup_base (scope_map::iterator,
              const dir_path& out_base,
              const dir_path& src_base);

  // Return a scope for the specified directory (first). Note that switching
  // to this scope might also involve switch to a new root scope (second) if
  // the new scope is in another project. If the new scope is not in any
  // project, then NULL is returned in second.
  //
  pair<scope&, scope*>
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
  scope&
  load_project (scope&,
                const dir_path& out_root, const dir_path& src_root,
                bool load = true);

  // Bootstrap the project's root scope, the out part.
  //
  void
  bootstrap_out (scope& root);

  // Bootstrap the project's root scope, the src part. Return true if
  // we loaded anything (which confirms the src_root is not bogus).
  //
  bool
  bootstrap_src (scope& root);

  // Return true if this scope has already been bootstrapped, that is, the
  // following calls have already been made:
  //
  //   bootstrap_out()
  //   setup_root()
  //   bootstrap_src()
  //
  bool
  bootstrapped (scope& root);

  // Create and bootstrap outer root scopes, if any. Loading is
  // done by load_root_pre() below.
  //
  void
  create_bootstrap_outer (scope& root);

  // Create and bootstrap inner root scopes between root and base,
  // if any. Return the innermost created root scope or root if
  // none were created. Loading is done by load_root_pre() below.
  //
  scope&
  create_bootstrap_inner (scope& root, const dir_path& out_base);

  // Load project's root[-pre].build unless already loaded. Also
  // make sure all outer root scopes are loaded prior to loading
  // this root scope.
  //
  void
  load_root_pre (scope& root);

  // Extract the specified variable value from a buildfile. It is expected to
  // be the first non-comment line and not to rely on any variable expansion
  // other than those from the global scope or any variable overrides. Return
  // an indication of whether the variable was found. The scope is only used
  // as proof of lock (though we don't modify anything).
  //
  pair<value, bool>
  extract_variable (scope&, const path&, const variable&);

  // Import has two phases: the first is triggered by the import
  // directive in the buildfile. It will try to find and load the
  // project. Failed that, it will return the project-qualified
  // name of the target which will be used to create a project-
  // qualified prerequisite. This gives the rule that will be
  // searching this prerequisite a chance to do some target-type
  // specific search. For example, a C++ link rule can search
  // for lib{} prerequisites in the C++ compiler default library
  // search paths (so that we end up with functionality identical
  // to -lfoo). If, however, the rule didn't do any of that (or
  // failed to find anything usable), it calls the standard
  // prerequisite search() function which sees this is a project-
  // qualified prerequisite and goes straight to the second phase
  // of import. Here, currently, we simply fail but in the future
  // this will be the place where we can call custom "last resort"
  // import hooks. For example, we can hook a package manager that
  // will say, "Hey, I see you are trying to import foo and I see
  // there is a package foo available in repository bar. Wanna
  // download and use it?"
  //
  names
  import (scope& base, name, const location&);

  const target&
  import (const prerequisite_key&);

  // As above but only imports as an already existing target. Unlike the above
  // version, this one can be called during the execute phase.
  //
  // Note: similar to search_existing().
  //
  const target*
  import_existing (const prerequisite_key&);
}

#include <build2/file.ixx>

#endif // BUILD2_FILE_HXX
