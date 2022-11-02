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
    // Optionally, execute the specified function at the end of the pipe,
    // either after the last command or instead of it.
    //
    void
    run (environment&,
         const command_expr&,
         const iteration_index*, size_t index,
         const location&,
         const function<command_function>& = nullptr,
         bool last_cmd = true);

    bool
    run_cond (environment&,
              const command_expr&,
              const iteration_index*, size_t index,
              const location&,
              const function<command_function>& = nullptr,
              bool last_cmd = true);

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

    // Read the stream content, optionally splitting the input data at
    // whitespaces or newlines and calling the specified callback function for
    // each substring (see the set builtin options for the splitting
    // semantics). Throw failed on io_error.
    //
    // If the stream is a pipeline's output, then the pipeline argument must
    // also be specified. Normally called from a custom command function (see
    // command_function for details) which is provided with the pipeline
    // information.
    //
    // Turn the stream into the non-blocking mode and, if the pipeline is
    // specified, read out its buffered stderr streams while waiting for the
    // input stream data. If a deadline is specified and is reached, then
    // terminate the whole pipeline, if specified, and bail out. Otherwise
    // issue diagnostics and fail. The thinking here is that in the former
    // case the caller first needs to dump the buffered stderr streams, issue
    // the appropriate diagnostics for the pipeline processes/builtins, and
    // only throw failed afterwards.
    //
    // Note that on Windows we can only turn file descriptors of the pipe type
    // into the non-blocking mode. Thus, a non-pipe descriptor is read in the
    // blocking manner (and the deadline is checked less accurately). This is
    // fine since there are no pipeline stderr streams to read out in this
    // case.
    //
    void
    read (auto_fd&&,
          bool whitespace, bool newline, bool exact,
          const function<void (string&&)>&,
          pipe_command* pipeline,
          const optional<deadline>&,
          const location&,
          const char* what);
  }
}

#endif // LIBBUILD2_SCRIPT_RUN_HXX
