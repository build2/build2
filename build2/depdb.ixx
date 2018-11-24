// file      : build2/depdb.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
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

#ifndef BUILD2_MTIME_CHECK
  inline void depdb::
  verify (const path_type&, timestamp)
  {
  }

  inline void depdb::
  verify (timestamp, const path_type&, const path_type&, timestamp)
  {
  }
#endif
}
