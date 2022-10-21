// file      : libbuild2/file-cache.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/file-cache.hxx>

#include <libbutl/lz4.hxx>

#include <libbuild2/filesystem.hxx>  // exists(), try_rmfile()
#include <libbuild2/diagnostics.hxx>

using namespace butl;

namespace build2
{
  // file_cache::entry
  //
  file_cache::write file_cache::entry::
  init_new ()
  {
    assert (state_ == uninit);

    // Remove stale compressed file if it exists. While not strictly necessary
    // (since the presence of the new uncompressed file will render the
    // compressed one invalid), this makes things cleaner in case we don't get
    // to compressing the new file (for example, if we fail and leave the
    // uncompressed file behind for troubleshooting).
    //
    if (!comp_path_.empty ())
      try_rmfile_ignore_error (comp_path_);

    // Note: state remains uninit until write::close().

    pin ();
    return write (*this);
  }

  void file_cache::entry::
  init_existing ()
  {
    assert (state_ == uninit);

    // Determine the cache state from the filesystem state.
    //
    // First check for the uncompressed file. Its presence means that the
    // compressed file, if exists, is invalid and we clean it up, similar to
    // init_new().
    //
    // Note that if compression is disabled, we omit the check assuming the
    // the uncompressed file exists.
    //
    if (!comp_path_.empty ())
    {
      if (exists (path_))
      {
        try_rmfile_ignore_error (comp_path_);
        state_ = uncomp;
      }
      else if (exists (comp_path_))
      {
        state_ = comp;
      }
      else
        fail << comp_path_ << " (or its uncompressed variant) does not exist" <<
          info << "consider cleaning the build state";
    }
    else
      state_ = uncomp;
  }

  void file_cache::entry::
  preempt ()
  {
    // Note that this function is called from destructors so it's best if it
    // doesn't throw.
    //
    switch (state_)
    {
    case uncomp:
      {
        if (!compress ())
          break;

        state_ = decomp; // We now have both.
      }
      // Fall through.
    case decomp:
      {
        if (try_rmfile_ignore_error (path_))
          state_ = comp;

        break;
      }
    default:
      assert (false);
    }
  }

  bool file_cache::entry::
  compress ()
  {
    tracer trace ("file_cache::entry::compress");

    try
    {
      ifdstream ifs (path_,      fdopen_mode::binary, ifdstream::badbit);
      ofdstream ofs (comp_path_, fdopen_mode::binary);

      uint64_t n (fdstat (ifs.fd ()).size);

      // Experience shows that for the type of content we typically cache
      // using 1MB blocks results in almost the same comression as for 4MB.
      //
      uint64_t cn (lz4::compress (ofs, ifs,
                                  1 /* compression_level (fastest) */,
                                  6 /* block_size_id (1MB) */,
                                  n));

      ofs.close ();

      l6 ([&]{trace << "compressed " << path_ << " to "
                    << (cn * 100 / n) << '%';});
    }
    catch (const std::exception& e)
    {
      l5 ([&]{trace << "unable to compress " << path_ << ": " << e;});
      try_rmfile_ignore_error (comp_path_);
      return false;
    }

    return true;
  }

  void file_cache::entry::
  decompress ()
  {
    try
    {
      ifdstream ifs (comp_path_, fdopen_mode::binary, ifdstream::badbit);
      ofdstream ofs (path_,      fdopen_mode::binary);

      lz4::decompress (ofs, ifs);

      ofs.close ();
    }
    catch (const std::exception& e)
    {
      fail << "unable to decompress " << comp_path_ << ": " << e <<
        info << "consider cleaning the build state";
    }
  }

  void file_cache::entry::
  remove ()
  {
    switch (state_)
    {
    case uninit:
      {
        // In this case we are cleaning the filesystem without having any idea
        // about its state. As a result, if we couldn't remove the compressed
        // file, then we don't attempt to remove the uncompressed file either
        // since it could be an indicator that the compressed file is invalid.
        //
        if (comp_path_.empty () || try_rmfile_ignore_error (comp_path_))
          try_rmfile_ignore_error (path_);
        break;
      }
    case uncomp:
      {
        try_rmfile_ignore_error (path_);
        break;
      }
    case comp:
      {
        try_rmfile_ignore_error (comp_path_);
        break;
      }
    case decomp:
      {
        // Both are valid so we are ok with failing to remove either.
        //
        try_rmfile_ignore_error (comp_path_);
        try_rmfile_ignore_error (path_);
        break;
      }
    case null:
      assert (false);
    }
  }
}
