// file      : libbuild2/cmdline.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CMDLINE_HXX
#define LIBBUILD2_CMDLINE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/b-options.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  struct cmdline
  {
    strings cmd_vars;
    string buildspec;
    uint16_t verbosity;
  };

  LIBBUILD2_SYMEXPORT cmdline
  parse_cmdline (tracer&, int argc, char* argv[], options&);
}

#endif // LIBBUILD2_CMDLINE_HXX
