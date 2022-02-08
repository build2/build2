// file      : libbuild2/depdb.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/diagnostics.hxx>

namespace build2
{
  inline depdb_base::
  ~depdb_base ()
  {
    if (state_ != state::write || ro_)
      is_.~ifdstream ();
    else
      os_.~ofdstream ();
  }

  inline void depdb::
  flush ()
  {
    if (state_ == state::write && !ro_)
    try
    {
      os_.flush ();
    }
    catch (const io_error& e)
    {
      fail << "unable to flush " << path << ": " << e;
    }
  }

  inline bool depdb::
  mtime_check ()
  {
    return mtime_check_option ? *mtime_check_option : LIBBUILD2_MTIME_CHECK;
  }

  inline void depdb::
  check_mtime (const path_type& t, timestamp e)
  {
    if (state_ == state::write && !ro_ && mtime_check ())
      check_mtime_ (t, e);
  }

  inline void depdb::
  check_mtime (timestamp s,
               const path_type& d,
               const path_type& t,
               timestamp e)
  {
    if (mtime_check ())
      check_mtime_ (s, d, t, e);
  }
}
