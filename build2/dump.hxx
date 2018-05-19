// file      : build2/dump.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_DUMP_HXX
#define BUILD2_DUMP_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  class scope;
  class target;

  // Dump the build state to diag_stream.
  //
  void
  dump ();

  void
  dump (const scope&, const char* ind = "");

  void
  dump (const target&, const char* ind = "");
}

#endif // BUILD2_DUMP_HXX
