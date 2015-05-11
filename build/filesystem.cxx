// file      : build/filesystem.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/filesystem>

#include <unistd.h>    // rmdir(), unlink()
#include <sys/types.h> // stat
#include <sys/stat.h>  // stat, lstat(), S_IS*, mkdir()

#include <system_error>

using namespace std;

namespace build
{
  bool
  dir_exists (const path& p)
  {
    struct stat s;
    if (::lstat (p.string ().c_str (), &s) != 0)
    {
      if (errno == ENOENT || errno == ENOTDIR)
        return false;
      else
        throw system_error (errno, system_category ());
    }

    return S_ISDIR (s.st_mode);
  }

  bool
  file_exists (const path& p)
  {
    struct stat s;
    if (::lstat (p.string ().c_str (), &s) != 0)
    {
      if (errno == ENOENT || errno == ENOTDIR)
        return false;
      else
        throw system_error (errno, system_category ());
    }

    return S_ISREG (s.st_mode);
  }

  mkdir_status
  try_mkdir (const path& p, mode_t m)
  {
    mkdir_status r (mkdir_status::success);

    if (::mkdir (p.string ().c_str (), m) != 0)
    {
      int e (errno);

      // EEXIST means the path already exists but not necessarily as
      // a directory.
      //
      if (e == EEXIST && dir_exists (p))
        return mkdir_status::already_exists;
      else
        throw system_error (e, system_category ());
    }

    return r;
  }

  rmdir_status
  try_rmdir (const path& p)
  {
    rmdir_status r (rmdir_status::success);

    if (::rmdir (p.string ().c_str ()) != 0)
    {
      if (errno == ENOENT)
        r = rmdir_status::not_exist;
      else if (errno == ENOTEMPTY || errno == EEXIST)
        r = rmdir_status::not_empty;
      else
        throw system_error (errno, system_category ());
    }

    return r;
  }

  rmfile_status
  try_rmfile (const path& p)
  {
    rmfile_status r (rmfile_status::success);

    if (::unlink (p.string ().c_str ()) != 0)
    {
      if (errno == ENOENT || errno == ENOTDIR)
        r = rmfile_status::not_exist;
      else
        throw system_error (errno, system_category ());
    }

    return r;
  }
}
