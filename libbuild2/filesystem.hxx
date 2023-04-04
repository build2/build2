// file      : libbuild2/filesystem.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_FILESYSTEM_HXX
#define LIBBUILD2_FILESYSTEM_HXX

#include <libbutl/filesystem.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/context.hxx>

#include <libbuild2/export.hxx>

// Higher-level filesystem utilities built on top of <libbutl/filesystem.hxx>.
//
// Compared to the libbutl's versions, these handle errors and issue
// diagnostics. Some of them also print the corresponding command line
// equivalent at the specified verbosity level. Note that most of such
// functions also respect the dry_run flag.
//
namespace build2
{
  using butl::entry_type;

  using butl::auto_rmfile;
  using butl::auto_rmdir;

  // The dual interface wrapper for the {mk,rm}{file,dir}() functions
  // below that allows you to use it as a true/false return or a more
  // detailed enum from <libbutl/filesystem.hxx>
  //
  template <typename T>
  struct fs_status
  {
    T v;
    fs_status (T v): v (v) {};
    operator T () const {return v;}
    explicit operator bool () const {return v == T::success;}
  };

  // Set the file access and modification times (unless dry-run) to the
  // current time printing the standard diagnostics starting from the
  // specified verbosity level. If the file does not exist and create is true,
  // create it and fail otherwise.
  //
  LIBBUILD2_SYMEXPORT void
  touch (context&, const path&, bool create, uint16_t verbosity = 1);

  // Return the modification time for an existing regular file and
  // timestamp_nonexistent otherwise. Print the diagnostics and fail on system
  // error.
  //
  LIBBUILD2_SYMEXPORT timestamp
  mtime (const char*);

  inline timestamp
  mtime (const path& p)
  {
    return mtime (p.string ().c_str ());
  }

  // Create the directory and print the standard diagnostics starting from the
  // specified verbosity level.
  //
  // Note that these functions ignore the dry_run flag (we might need to save
  // something in such a directory, such as depdb, ignoring dry_run). Overall,
  // it feels like we should establish the structure even for dry-run.
  //
  // Note that the implementation may not be suitable if the performance is
  // important and it is expected that the directory will exist in most cases.
  // See the fsdir{} rule for details.
  //
  using mkdir_status = butl::mkdir_status;

  LIBBUILD2_SYMEXPORT fs_status<mkdir_status>
  mkdir (const dir_path&, uint16_t verbosity);

  LIBBUILD2_SYMEXPORT fs_status<mkdir_status>
  mkdir_p (const dir_path&, uint16_t verbosity);

  // Rename a file (or file symlink) overwriting the destination if exists.
  //
  LIBBUILD2_SYMEXPORT void
  mvfile (const path& from, const path& to, uint16_t verbosity = 1);

  // Remove the file (unless dry-run) and print the standard diagnostics
  // starting from the specified verbosity level. The second argument is only
  // used in diagnostics, to print the target name. Passing the path for
  // target will result in the relative path being printed.
  //
  using rmfile_status = butl::rmfile_status;

  template <typename T>
  fs_status<rmfile_status>
  rmfile (const path&, const T& target, uint16_t verbosity = 1);

  fs_status<rmfile_status>
  rmfile (context&, const path&, uint16_t verbosity = 1);

  fs_status<rmfile_status>
  rmfile (const path&, int = 1) = delete;

  fs_status<rmfile_status>
  rmfile (const path&, uint16_t) = delete;

  // Similar to rmfile() but for symlinks.
  //
  LIBBUILD2_SYMEXPORT fs_status<rmfile_status>
  rmsymlink (context&, const path&, bool dir, uint16_t verbosity);

  // Similar to rmfile() but for directories (note: not -r).
  //
  using rmdir_status = butl::rmdir_status;

  template <typename T>
  fs_status<rmdir_status>
  rmdir (const dir_path&, const T& target, uint16_t verbosity = 1);

  fs_status<rmdir_status>
  rmdir (context&, const dir_path&, uint16_t verbosity = 1);

  fs_status<rmdir_status>
  rmdir (const dir_path&, int = 1) = delete;

  fs_status<rmdir_status>
  rmdir (const dir_path&, uint16_t) = delete;

  // Remove the directory recursively (unless dry-run) and print the standard
  // diagnostics starting from the specified verbosity level. Note that this
  // function returns not_empty if we try to remove a working directory. If
  // the dir argument is false, then the directory itself is not removed.
  //
  LIBBUILD2_SYMEXPORT fs_status<rmdir_status>
  rmdir_r (context& ctx,
           const dir_path&, bool dir = true, uint16_t verbosity = 1);

  fs_status<rmdir_status>
  rmdir_r (const dir_path&, bool = true, uint16_t = 1) = delete;

  // Check for a file, directory or filesystem entry existence. Print the
  // diagnostics and fail on system error, unless ignore_error is true.
  //
  LIBBUILD2_SYMEXPORT bool
  exists (const path&, bool follow_symlinks = true, bool ignore_error = false);

  LIBBUILD2_SYMEXPORT bool
  exists (const dir_path&, bool ignore_error = false);

  LIBBUILD2_SYMEXPORT bool
  entry_exists (const path&,
                bool follow_symlinks = false,
                bool ignore_error = false);

  // Check for a directory emptiness. Print the diagnostics and fail on system
  // error.
  //
  LIBBUILD2_SYMEXPORT bool
  empty (const dir_path&);

  // Directories containing .buildignore (or .build2ignore in the alternative
  // naming scheme) file are automatically ignored by recursive name patterns.
  // For now the file is just a marker and its contents don't matter. Note
  // that these functions ignore dry-run.

  // Create a directory containing an empty .buildignore file.
  //
  LIBBUILD2_SYMEXPORT fs_status<mkdir_status>
  mkdir_buildignore (context&,
                     const dir_path&, const path&, uint16_t verbosity);

  // Return true if the directory is empty or only contains the .buildignore
  // file. Fail if the directory doesn't exist.
  //
  LIBBUILD2_SYMEXPORT bool
  empty_buildignore (const dir_path&, const path&);

  // Remove a directory if it is empty or only contains the .buildignore file.
  //
  LIBBUILD2_SYMEXPORT fs_status<rmdir_status>
  rmdir_buildignore (context&,
                     const dir_path&, const path&, uint16_t verbosity = 1);

  // Get/set a path permissions.
  //
  using permissions = butl::permissions;

  LIBBUILD2_SYMEXPORT permissions
  path_perms (const path&);

  LIBBUILD2_SYMEXPORT void
  path_perms (const path&, permissions);

  // Normalize an absolute path to an existing file that may reside outside of
  // any project and could involve funny filesystem business (e.g., relative
  // directory symlinks). For example, a C/C++ header path returned by a
  // compiler which could be a system header.
  //
  // We used to just normalize such a path but that could result in an invalid
  // path (e.g., for some system/compiler headers on CentOS 7 with Clang 3.4)
  // because of the symlinks (if a directory component is a symlink, then any
  // following `..` are resolved relative to the target; see path::normalize()
  // for background).
  //
  // Initially, to fix this, we realized (i.e., realpath(3)) it instead. But
  // that turned out also not to be quite right since now we have all the
  // symlinks resolved: conceptually it feels correct to keep the original
  // header names since that's how the user chose to arrange things and
  // practically this is how compilers see/report them (e.g., the GCC module
  // mapper).
  //
  // So now we have a pretty elaborate scheme where we try to use the
  // normalized path if possible and fallback to realized. Normalized paths
  // will work for situations where `..` does not cross symlink boundaries,
  // which is the sane case. And for the insane case we only really care
  // about out-of-project files (i.e., system/compiler headers). In other
  // words, if you have the insane case inside your project, then you are on
  // your own.
  //
  LIBBUILD2_SYMEXPORT void
  normalize_external (path&, const char* what);
}

#include <libbuild2/filesystem.ixx>
#include <libbuild2/filesystem.txx>

#endif // LIBBUILD2_FILESYSTEM_HXX
