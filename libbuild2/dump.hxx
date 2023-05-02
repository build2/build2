// file      : libbuild2/dump.hxx -*- C++ -*-
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
  // If scope or target is NULL, then assume not found and write a format-
  // appropriate indication.
  //
  LIBBUILD2_SYMEXPORT void
  dump (const context&, optional<action>);

  LIBBUILD2_SYMEXPORT void
  dump (const scope*, optional<action>, const char* ind = "");

  LIBBUILD2_SYMEXPORT void
  dump (const target*, optional<action>, const char* ind = "");
}

#endif // LIBBUILD2_DUMP_HXX
