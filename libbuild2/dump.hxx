// file      : libbuild2/dump.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DUMP_HXX
#define LIBBUILD2_DUMP_HXX

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/serializer.hxx>
#endif

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  enum class dump_format {buildfile, json};

  // Dump the build state to diag_stream. If action is specified, then assume
  // rules have been matched for this action and dump action-specific
  // information (like rule-specific variables).
  //
  // If scope or target is NULL, then assume not found and write a format-
  // appropriate indication.
  //
  LIBBUILD2_SYMEXPORT void
  dump (const context&, optional<action>, dump_format);

  LIBBUILD2_SYMEXPORT void
  dump (const scope*, optional<action>, dump_format, const char* ind = "");

  LIBBUILD2_SYMEXPORT void
  dump (const target*, optional<action>, dump_format, const char* ind = "");

#ifndef BUILD2_BOOTSTRAP
  // Dump (effectively) quoted target name, optionally relative (to the out
  // tree).
  //
  LIBBUILD2_SYMEXPORT void
  dump_quoted_target_name (butl::json::stream_serializer&,
                           const target&,
                           bool relative = false);

  // Dump display target name, optionally relative (to the out tree).
  //
  LIBBUILD2_SYMEXPORT void
  dump_display_target_name (butl::json::stream_serializer&,
                            const target&,
                            bool relative = false);
#endif
}

#endif // LIBBUILD2_DUMP_HXX
