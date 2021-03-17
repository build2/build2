// file      : libbuild2/file-cache.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/filesystem.hxx> // try_rmfile()

namespace build2
{
  // file_cache::write
  //
  inline void file_cache::write::
  close ()
  {
  }

  // file_cache::read
  //
  inline file_cache::read::
  ~read ()
  {
  }

  // file_cache::entry
  //
  inline const path& file_cache::entry::
  path () const
  {
    return path_;
  }

  inline file_cache::write file_cache::entry::
  init_new ()
  {
    return write ();
  }

  inline void file_cache::entry::
  init_existing ()
  {
  }

  inline file_cache::read file_cache::entry::
  open ()
  {
    return read ();
  }

  inline void file_cache::entry::
  pin ()
  {
  }

  inline void file_cache::entry::
  unpin ()
  {
  }

  inline file_cache::entry::
  operator bool () const
  {
    return !path_.empty ();
  }

  inline file_cache::entry::
  entry (path_type p, bool t)
      : temporary (t), path_ (move (p))
  {
  }

  inline file_cache::entry::
  ~entry ()
  {
    if (!path_.empty () && temporary)
      try_rmfile (path_, true /* ignore_errors */);
  }

  inline file_cache::entry::
  entry (entry&& e)
      : temporary (e.temporary), path_ (move (e.path_))
  {
  }

  inline file_cache::entry& file_cache::entry::
  operator= (entry&& e)
  {
    if (this != &e)
    {
      assert (path_.empty ());
      temporary = e.temporary;
      path_ = move (e.path_);
    }
    return *this;
  }

  // file_cache
  //
  inline file_cache::entry file_cache::
  create (path f, optional<bool>)
  {
    return entry (move (f), true /* temporary */);
  }

  inline file_cache::entry file_cache::
  create_existing (path f)
  {
    entry e (move (f), false /* temporary */);
    e.init_existing ();
    return e;
  }

  inline string file_cache::
  compressed_extension (const char*)
  {
    return string ();
  }

  inline file_cache::
  file_cache (scheduler&)
  {
  }
}
