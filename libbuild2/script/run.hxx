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

    // Read out the stream content into a string, optionally splitting the
    // input data at whitespaces or newlines in which case return one
    // sub-string at a time (see the set builtin options for the splitting
    // semantics). Throw io_error on the underlying OS error.
    //
    // If the execution deadline is specified, then turn the stream into the
    // non-blocking mode. If the specified deadline is reached while reading
    // the stream, then bail out for the successful deadline and fail
    // otherwise. Note that in the former case the result will be incomplete,
    // but we leave it to the caller to handle that.
    //
    // Note that on Windows we can only turn pipe file descriptors into the
    // non-blocking mode. Thus, we have no choice but to read from descriptors
    // of other types synchronously there. That implies that we can
    // potentially block indefinitely reading a file and missing the deadline
    // on Windows. Note though, that the user can normally rewrite the
    // command, for example, `set foo <<<file` with `cat file | set foo` to
    // avoid this problem.
    //
    class stream_reader
    {
    public:
      stream_reader (auto_fd&&,
                     bool pipe,
                     bool whitespace, bool newline, bool exact,
                     const optional<deadline>&,
                     const command& deadline_cmd,
                     const location&);

      // Return nullopt if eos is reached.
      //
      optional<string>
      next ();

    private:
      ifdstream is_;
      bool whitespace_;
      bool newline_;
      bool exact_;
      optional<deadline> deadline_;
      const command& deadline_cmd_;
      const location& location_;

      bool empty_ = true; // Set to false after the first character is read.
    };

    // Read the stream content using the stream reader in the no-split exact
    // mode.
    //
    string
    stream_read (auto_fd&&,
                 bool pipe,
                 const optional<deadline>&,
                 const command& deadline_cmd,
                 const location&);
  }
}

#endif // LIBBUILD2_SCRIPT_RUN_HXX
