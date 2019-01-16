// file      : build2/depdb.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline depdb_base::
  ~depdb_base ()
  {
    if (state_ != state::write)
      is_.~ifdstream ();
    else
      os_.~ofdstream ();
  }

  inline bool depdb::
  mtime_check ()
  {
    // Note: options were validated in main().
    //
    return (ops.   mtime_check () ? true  :
            ops.no_mtime_check () ? false :
            BUILD2_MTIME_CHECK);
  }

  inline void depdb::
  check_mtime (const path_type& t, timestamp e)
  {
    if (state_ == state::write && mtime_check ())
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
