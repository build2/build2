// file      : libbuild2/file.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_FILE_HXX
#define LIBBUILD2_FILE_HXX

#include <map>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx> // list_value

#include <libbuild2/export.hxx>

namespace build2
{
  class lexer;

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

  // Project's loading stage during which the parsing is performed.
  //
  enum class load_stage
  {
    boot,  // Loading bootstrap.build (or bootstrap pre/post hooks).
    root,  // Loading root.build (or root pre/post hooks).
    rest   // Loading the rest (ordinary buildfiles, command line, etc).
  };

  // If buildfile is '-', then read from STDIN.
  //
  LIBBUILD2_SYMEXPORT void
  source (scope& root, scope& base, const path&);

  // As above, but extract from a stream. The name argument is used for
  // diagnostics.
  //
  LIBBUILD2_SYMEXPORT void
  source (scope& root, scope& base, istream&, const path_name&);

  // As above, but extract from a lexer (this could be useful for sourcing
  // stdin that requires parse_variable()).
  //
  LIBBUILD2_SYMEXPORT void
  source (scope& root, scope& base, lexer&, load_stage = load_stage::rest);

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
  // src_root value is not empty.
  //
  LIBBUILD2_SYMEXPORT scope_map::iterator
  create_root (context&, const dir_path& out_root, const dir_path& src_root);

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

  // Return a scope for the specified directory (first). If project is true
  // then switching to this scope might also involve switch to a new root
  // scope (second) if the new scope is in another project. If project is
  // false or the new scope is not in any project, then NULL is returned in
  // second.
  //
  LIBBUILD2_SYMEXPORT pair<scope&, scope*>
  switch_scope (scope& root, const dir_path&, bool project = true);

  // Bootstrap and optionally load an ad hoc (sub)project (i.e., the kind that
  // is not discovered and loaded automatically by bootstrap/load functions
  // above).
  //
  // Note that we expect the outer project (if any) to be bootstrapped and
  // loaded and currently we do not add the newly loaded subproject to the
  // outer project's subprojects map.
  //
  LIBBUILD2_SYMEXPORT scope&
  load_project (context&,
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

  // Bootstrap the project's root scope, the out part. Return the src_root
  // variable value (which can be NULL).
  //
  LIBBUILD2_SYMEXPORT value&
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
  // be the first non-blank/comment line and not to rely on any variable
  // expansions other than those from the global scope or any variable
  // overrides. Return nullopt if the variable was not found.
  //
  LIBBUILD2_SYMEXPORT optional<value>
  extract_variable (context&, const path&, const variable&);

  // As above, but extract from a stream. The path argument is used for
  // diagnostics.
  //
  LIBBUILD2_SYMEXPORT optional<value>
  extract_variable (context&, istream&, const path&, const variable&);

  // As above, but extract from a lexer (this could be useful for extracting
  // from stdin).
  //
  LIBBUILD2_SYMEXPORT optional<value>
  extract_variable (context&, lexer&, const variable&);

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
  // currently, we only have special handling of exe{} targets (search in
  // PATH) simply failing for the rest. But in the future this coud be the
  // place where we could call custom "last resort" import hooks. For example,
  // we can hook a package manager that will say, "Hey, dude, I see you are
  // trying to import foo and I see there is a package foo available in
  // repository bar. Wanna, like, download and use it or something?" Though
  // the latest thoughts indicate this is probably a bad idea (implicitness,
  // complexity, etc).
  //
  // More specifically, we have the following kinds of import (tried in this
  // order):
  //
  // ad hoc
  //
  //  The target is imported by specifying its path directly with
  //  config.import.<proj>.<name>[.<type>]. For example, this can be
  //  used to import an installed target.
  //
  //
  // normal
  //
  //  The target is imported from a project that was either specified with
  //  config.import.<proj> or was found via the subproject search. This also
  //  loads the target's dependency information.
  //
  //
  // rule-specific
  //
  //  The target was imported in a rule-specific manner (e.g., a library was
  //  found in the compiler's search paths).
  //
  //
  // fallback/default
  //
  //  The target was found by the second phase of import (e.g., an executable
  //  was found in PATH).

  // Import phase 1. Return the imported target(s) as well as the kind of
  // import that was performed with `fallback` indicating it was not found.
  //
  // If second is `fallback`, then first contains the original, project-
  // qualified target. If second is `adhoc`, first may still contain a
  // project-qualified target (which may or may not be the same as the
  // original; see the config.import.<proj>.<name>[.<type>] logic for details)
  // in which case it should still be passed to import phase 2.
  //
  // If phase2 is true then the phase 2 is performed right away (we call it
  // immediate import). Note that if optional is true, phase2 must be true as
  // well (and thus there is no rule-specific logic for optional imports). In
  // case of optional, empty names value is retuned if nothing was found.
  //
  // If metadata is true, then load the target metadata. In this case phase2
  // must be true as well.
  //
  // Note also that we return names rather than a single name: while normally
  // it will be a single target name, it can be an out-qualified pair (if
  // someone wants to return a source target) but it can also be a non-target
  // since we don't restrict what users can import/export.
  //
  // Finally, note that import is (and should be kept) idempotent or, more
  // precisely, "accumulatively idempotent" in that additional steps may be
  // performed (phase 2, loading of the metadata) unless already done.
  //
  enum class import_kind {adhoc, normal, fallback};

  LIBBUILD2_SYMEXPORT pair<names, import_kind>
  import (scope& base,
          name,
          bool phase2,
          bool optional,
          bool metadata,
          const location&);

  // Import phase 2.
  //
  const target&
  import (context&, const prerequisite_key&);

  // As above but import the target "here and now" without waiting for phase 2
  // (and thus omitting any rule-specific logic). This version of import is,
  // for example, used by build system modules to perform an implicit import
  // of the corresponding tool.
  //
  // If phase2 is false, then the second phase's fallback/default logic is
  // only invoked if the import was ad hoc (i.e., a relative path was
  // specified via config.import.<proj>.<name>[.<type>]) with NULL returned
  // otherwise.
  //
  // If phase2 is true and optional is true, then NULL is returned instead of
  // failing if phase 2 could not find anything.
  //
  // If metadata is true, then load the target metadata. In this case phase2
  // must be true as well.
  //
  // The what argument specifies what triggered the import (for example,
  // "module load") and is used in diagnostics.
  //
  // This function also returns the kind of import that was performed.
  //
  pair<const target*, import_kind>
  import_direct (scope& base,
                 name,
                 bool phase2,
                 bool optional,
                 bool metadata,
                 const location&,
                 const char* what = "import");

  // As above but also return (in new_value) an indication of whether this
  // import is based on a new config.* value. See config::lookup_config() for
  // details. Note that a phase 2 fallback/default logic is not considered new
  // (though this can be easily adjusted based on import kind).
  //
  LIBBUILD2_SYMEXPORT pair<const target*, import_kind>
  import_direct (bool& new_value,
                 scope& base,
                 name,
                 bool phase2,
                 bool optional,
                 bool metadata,
                 const location&,
                 const char* what = "import");


  template <typename T>
  pair<const T*, import_kind>
  import_direct (scope&,
                 name, bool, bool, bool,
                 const location&, const char* = "import");

  template <typename T>
  pair<const T*, import_kind>
  import_direct (bool&,
                 scope&,
                 name,
                 bool, bool, bool,
                 const location&, const char* = "import");

  // Print import_direct<exe>() result either as a target for a normal import
  // or as a process path for ad hoc and fallback imports. Normally used in
  // build system modules to print the configuration report.
  //
  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const pair<const exe*, import_kind>&);

  // As import phase 2 but only imports as an already existing target. But
  // unlike it, this function can be called during the load and execute
  // phases.
  //
  // Note: similar to search_existing().
  //
  const target*
  import_existing (context&, const prerequisite_key&);

  // Lower-level components of phase 1 (see implementation for details).
  //
  pair<name, optional<dir_path>>
  import_search (scope& base,
                 name,
                 bool optional_,
                 const optional<string>& metadata, // False or metadata key.
                 bool subprojects,
                 const location&,
                 const char* what = "import");

  // As above but also return (in new_value) an indication of whether this
  // import is based on a new config.* value. See config::lookup_config()
  // for details.
  //
  LIBBUILD2_SYMEXPORT pair<name, optional<dir_path>>
  import_search (bool& new_value,
                 scope& base,
                 name,
                 bool optional_,
                 const optional<string>& metadata,
                 bool subprojects,
                 const location&,
                 const char* what = "import");

  LIBBUILD2_SYMEXPORT pair<names, const scope&>
  import_load (context&,
               pair<name, optional<dir_path>>,
               bool metadata,
               const location&);

  // Create a build system project in the specified directory.
  //
  LIBBUILD2_SYMEXPORT void
  create_project (
    const dir_path&,
    const optional<dir_path>& amalgamation,
    const strings& boot_modules,           // Bootstrap modules.
    const string&  root_pre,               // Extra root.build text.
    const strings& root_modules,           // Root modules.
    const string&  root_post,              // Extra root.build text.
    const optional<string>& config_module, // Config module to load.
    const optional<string>& config_file,   // Ad hoc config.build contents.
    bool buildfile,                        // Create root buildfile.
    const char* who,                       // Who is creating it.
    uint16_t verbosity = 1);               // Diagnostic verbosity.
}

#include <libbuild2/file.ixx>

#endif // LIBBUILD2_FILE_HXX
