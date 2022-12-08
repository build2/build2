// file      : libbuild2/b-cmdline.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_B_CMDLINE_HXX
#define LIBBUILD2_B_CMDLINE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/b-options.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  struct b_cmdline
  {
    strings cmd_vars;
    string buildspec;

    // Processed/meged option values (unless --help or --version specified).
    //
    uint16_t verbosity = 1;
    optional<bool> progress;
    optional<bool> diag_color;
    optional<bool> mtime_check;
    optional<path> config_sub;
    optional<path> config_guess;
    size_t jobs = 0;
    size_t max_jobs = 0;
    optional<size_t> max_stack;
    bool fcache_compress = true;
  };

  LIBBUILD2_SYMEXPORT b_cmdline
  parse_b_cmdline (tracer&,
                   int argc, char* argv[],
                   b_options&,
                   uint16_t default_verbosity = 1,
                   size_t default_jobs = 0);
}

#endif // LIBBUILD2_B_CMDLINE_HXX
