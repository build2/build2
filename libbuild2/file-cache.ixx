// file      : libbuild2/file-cache.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/filesystem.hxx> // try_rmfile()

namespace build2
{
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
}
