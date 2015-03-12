// file      : build/filesystem.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/filesystem>

#include <unistd.h>   // rmdir(), unlink()
#include <sys/stat.h> // mkdir()

#include <system_error>

using namespace std;

namespace build
{
  void
  mkdir (const path& p, mode_t m)
  {
    if (::mkdir (p.string ().c_str (), m) != 0)
      throw system_error (errno, system_category ());
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
