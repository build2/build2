// file      : libbuild2/filesystem.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_FILESYSTEM_HXX
#define LIBBUILD2_FILESYSTEM_HXX

#include <libbutl/filesystem.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

// Higher-level filesystem utilities built on top of <libbutl/filesystem.mxx>.
//
// Compared to the libbutl's versions, these handle errors and issue
// diagnostics. Some of them also print the corresponding command line
// equivalent at the specified verbosity level. Note that most of such
// functions also respect the dry_run flag.
//
namespace build2
{
  using butl::auto_rmfile;
  using butl::auto_rmdir;

  // The dual interface wrapper for the {mk,rm}{file,dir}() functions
  // below that allows you to use it as a true/false return or a more
  // detailed enum from <libbutl/filesystem.mxx>
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
  touch (const path&, bool create, uint16_t verbosity = 1);

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
  mkdir (const dir_path&, uint16_t verbosity = 1);

  LIBBUILD2_SYMEXPORT fs_status<mkdir_status>
  mkdir_p (const dir_path&, uint16_t verbosity = 1);

  // Remove the file (unless dry-run) and print the standard diagnostics
  // starting from the specified verbosity level. The second argument is only
  // used in diagnostics, to print the target name. Passing the path for
  // target will result in the relative path being printed.
  //
  using rmfile_status = butl::rmfile_status;

  template <typename T>
  fs_status<rmfile_status>
  rmfile (const path&, const T& target, uint16_t verbosity = 1);

  inline fs_status<rmfile_status>
  rmfile (const path& f, int verbosity = 1) // Literal overload (int).
  {
    return rmfile (f, f, static_cast<uint16_t> (verbosity));
  }

  inline fs_status<rmfile_status>
  rmfile (const path& f, uint16_t verbosity) // Overload (verb_never).
  {
    return rmfile (f, f, verbosity);
  }

  // Similar to rmfile() but for symlinks.
  //
  LIBBUILD2_SYMEXPORT fs_status<rmfile_status>
  rmsymlink (const path&, bool dir, uint16_t verbosity);

  // Similar to rmfile() but for directories (note: not -r).
  //
  using rmdir_status = butl::rmdir_status;

  template <typename T>
  fs_status<rmdir_status>
  rmdir (const dir_path&, const T& target, uint16_t verbosity = 1);

  inline fs_status<rmdir_status>
  rmdir (const dir_path& d, int verbosity = 1) // Literal overload (int).
  {
    return rmdir (d, d, static_cast<uint16_t> (verbosity));
  }

  inline fs_status<rmdir_status>
  rmdir (const dir_path& d, uint16_t verbosity) // Overload (verb_never).
  {
    return rmdir (d, d, verbosity);
  }

  // Remove the directory recursively (unless dry-run) and print the standard
  // diagnostics starting from the specified verbosity level. Note that this
  // function returns not_empty if we try to remove a working directory. If
  // the dir argument is false, then the directory itself is not removed.
  //
  // @@ Collides (via ADL) with butl::rmdir_r(), which sucks.
  //
  LIBBUILD2_SYMEXPORT fs_status<rmdir_status>
  rmdir_r (const dir_path&, bool dir = true, uint16_t verbosity = 1);

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
  mkdir_buildignore (const dir_path&, const path&, uint16_t verbosity = 1);

  // Return true if the directory is empty or only contains the .buildignore
  // file. Fail if the directory doesn't exist.
  //
  LIBBUILD2_SYMEXPORT bool
  empty_buildignore (const dir_path&, const path&);

  // Remove a directory if it is empty or only contains the .buildignore file.
  //
  LIBBUILD2_SYMEXPORT fs_status<rmdir_status>
  rmdir_buildignore (const dir_path&, const path&, uint16_t verbosity = 1);
}

#include <libbuild2/filesystem.txx>

#endif // LIBBUILD2_FILESYSTEM_HXX
