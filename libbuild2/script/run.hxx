// file      : libbuild2/script/run.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCRIPT_RUN_HXX
#define LIBBUILD2_SCRIPT_RUN_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/script/script.hxx>

namespace build2
{
  namespace script
  {
    // An exception that can be thrown by an expression running function to
    // exit the script (for example, as a result of executing the exit builtin
    // by the below run*() functions). The status indicates whether the
    // execution should be considered to have succeeded or failed.
    //
    struct exit
    {
      bool status;

      explicit
      exit (bool s): status (s) {}
    };

    // Helpers.
    //

    // Command expression running functions.
    //
    // Index is the 1-base index of this command line in the command list.
    // If it is 0 then it means there is only one command. This information
    // can be used, for example, to derive file names.
    //
    // Location is the start position of this command line in the script. It
    // can be used in diagnostics.
    //
    void
    run (environment&, const command_expr&, size_t index, const location&);

    bool
    run_if (environment&, const command_expr&, size_t, const location&);

    // Perform the registered special file cleanups in the direct order and
    // then the regular cleanups in the reverse order.
    //
    void
    clean (environment&, const location&);

    // Print first 10 directory sub-entries to the diag record. The directory
    // must exist. Is normally used while issuing diagnostics on non-empty
    // directory removal failure.
    //
    void
    print_dir (diag_record&, const dir_path&, const location&);

    // Return the quoted path representation with the preserved trailing
    // directory separator. The path is relative if the verbosity level is
    // less than 3.
    //
    string
    diag_path (const path&);

    // Same as above, but prepends the path with a name, if present. The path
    // must be not NULL.
    //
    string
    diag_path (const dir_name_view&);
  }
}

#endif // LIBBUILD2_SCRIPT_RUN_HXX
