// file      : libbuild2/dump.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DUMP_HXX
#define LIBBUILD2_DUMP_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Dump the build state to diag_stream. If action is specified, then assume
  // rules have been matched for this action and dump action-specific
  // information (like rule-specific variables).
  //
  LIBBUILD2_SYMEXPORT void
  dump (const context&, optional<action> = nullopt);

  LIBBUILD2_SYMEXPORT void
  dump (const scope&, const char* ind = "");

  LIBBUILD2_SYMEXPORT void
  dump (const target&, const char* ind = "");
}

#endif // LIBBUILD2_DUMP_HXX
