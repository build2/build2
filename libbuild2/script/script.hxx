// file      : libbuild2/script/script.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCRIPT_SCRIPT_HXX
#define LIBBUILD2_SCRIPT_SCRIPT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/token.hxx>
#include <libbuild2/variable.hxx>

namespace build2
{
  namespace script
  {
    // Pre-parsed representation.
    //

    enum class line_type
    {
      var,
      cmd,
      cmd_if,
      cmd_ifn,
      cmd_elif,
      cmd_elifn,
      cmd_else,
      cmd_while,
      cmd_for_args,   // `for x: ...`
      cmd_for_stream, // `... | for x` and `for x <...`
      cmd_end
    };

    ostream&
    operator<< (ostream&, line_type);

    struct line
    {
      line_type type;
      replay_tokens tokens;

      union
      {
        const variable* var; // Pre-entered for line_type::{var,cmd_for_*}.
      };
    };

    // Most of the time we will have just one line (a command).
    //
    using lines = small_vector<line, 1>;

    // Print the script lines, trying to reproduce their original (non-
    // expanded) representation.
    //
    // Note that the exact spacing and partial quoting may not be restored due
    // to the information loss.
    //
    void
    dump (ostream&, const string& ind, const lines&);

    // As above but print a single line and without the trailing newline token
    // by default.
    //
    void
    dump (ostream&, const line&, bool newline = false);

    // Parse object model.
    //

    // redirect
    //
    enum class redirect_type
    {
      // No data is allowed to be read or written.
      //
      // Note that redirect of this type cannot be currently specified on the
      // script command line and can only be set via the environment object
      // as a default redirect (see below).
      //
      none,

      pass,
      null,
      trace,
      merge,
      here_str_literal,
      here_str_regex,
      here_doc_literal,
      here_doc_regex,
      here_doc_ref,     // Reference to here_doc literal or regex.
      file,
    };

    // Pre-parsed (but not instantiated) regex lines. The idea here is that
    // we should be able to re-create their (more or less) exact text
    // representation for diagnostics but also instantiate without any
    // re-parsing.
    //
    struct regex_line
    {
      // If regex is true, then value is the regex expression. Otherwise, it
      // is a literal. Note that special characters can be present in both
      // cases. For example, //+ is a regex, while /+ is a literal, both
      // with '+' as a special character. Flags are only valid for regex.
      // Literals falls apart into textual (has no special characters) and
      // special (has just special characters instead) ones. For example
      // foo is a textual literal, while /.+ is a special one. Note that
      // literal must not have value and special both non-empty.
      //
      bool regex;

      string value;
      string flags;
      string special;

      uint64_t line;
      uint64_t column;

      // Create regex with optional special characters.
      //
      regex_line (uint64_t l, uint64_t c,
                  string v, string f, string s = string ())
          : regex (true),
            value (move (v)),
            flags (move (f)),
            special (move (s)),
            line (l),
            column (c) {}

      // Create a literal, either text or special.
      //
      regex_line (uint64_t l, uint64_t c, string v, bool s)
          : regex (false),
            value (s ? string () : move (v)),
            special (s ? move (v) : string ()),
            line (l),
            column (c) {}
    };

    struct regex_lines
    {
      char intro;   // Introducer character.
      string flags; // Global flags (here-document).

      small_vector<regex_line, 8> lines;
    };

    // Output file redirect mode.
    //
    enum class redirect_fmode
    {
      compare,
      overwrite,
      append
    };

    struct redirect
    {
      redirect_type type;

      struct file_type
      {
        using path_type = build2::path;
        path_type path;
        redirect_fmode mode; // Meaningless for input redirect.
      };

      union
      {
        int         fd;    // Merge-to descriptor.
        string      str;   // Note: with trailing newline, if requested.
        regex_lines regex; // Note: with trailing blank, if requested.
        file_type   file;
        reference_wrapper<const redirect> ref; // Note: no chains.
      };

      // Modifiers and the original representation (potentially an alias).
      //
      build2::token token;

      string end;         // Here-document end marker (no regex intro/flags).
      uint64_t end_line;  // Here-document end marker location.
      uint64_t end_column;

      // Create redirect of a type other than reference.
      //
      explicit
      redirect (redirect_type);

      // Create redirect of the reference type.
      //
      redirect (redirect_type t, const redirect& r, build2::token tk)
          : type (redirect_type::here_doc_ref),
            ref (r),
            token (move (tk))
      {
        // There is no support (and need) for reference chains.
        //
        assert (t == redirect_type::here_doc_ref &&
                r.type != redirect_type::here_doc_ref);
      }

      // Create redirect of the merge type.
      //
      // Note that it's the caller's responsibility to make sure that the file
      // descriptor is valid for this redirect (2 for stdout, etc).
      //
      redirect (redirect_type t, int f)
          : type (redirect_type::merge), fd (f)
      {
        assert (t == redirect_type::merge && (f == 1 || f == 2));
      }

      // Movable-only type.
      //
      redirect (redirect&&) noexcept;
      redirect& operator= (redirect&&) noexcept;

      redirect (const redirect&) = delete;
      redirect& operator= (const redirect&) = delete;

      ~redirect ();

      const redirect&
      effective () const noexcept
      {
        return type == redirect_type::here_doc_ref ? ref.get () : *this;
      }

      const string&
      modifiers () const noexcept
      {
        return token.value;
      }
    };

    // cleanup
    //
    enum class cleanup_type
    {
      always, // &foo  - cleanup, fail if does not exist.
      maybe,  // &?foo - cleanup, ignore if does not exist.
      never   // &!foo - don’t cleanup, ignore if doesn’t exist.
    };

    // File or directory to be automatically cleaned up at the end of the
    // script execution. If the path ends with a trailing slash, then it is
    // assumed to be a directory, otherwise -- a file. A directory that is
    // about to be cleaned up must be empty.
    //
    // The last component in the path may contain a wildcard that have the
    // following semantics:
    //
    // dir/*   - remove all immediate files
    // dir/*/  - remove all immediate sub-directories (must be empty)
    // dir/**  - remove all files recursively
    // dir/**/ - remove all sub-directories recursively (must be empty)
    // dir/*** - remove directory dir with all files and sub-directories
    //           recursively
    //
    struct cleanup
    {
      cleanup_type type;
      build2::path path;
    };
    using cleanups = small_vector<cleanup, 1>;

    // command_exit
    //
    enum class exit_comparison {eq, ne};

    struct command_exit
    {
      // C/C++ don't apply constraints on program exit code other than it
      // being of type int.
      //
      // POSIX specifies that only the least significant 8 bits shall be
      // available from wait() and waitpid(); the full value shall be
      // available from waitid() (read more at _Exit, _exit Open Group
      // spec).
      //
      // While the Linux man page for waitid() doesn't mention any
      // deviations from the standard, the FreeBSD implementation (as of
      // version 11.0) only returns 8 bits like the other wait*() calls.
      //
      // Windows supports 32-bit exit codes.
      //
      // Note that in shells some exit values can have special meaning so
      // using them can be a source of confusion. For bash values in the
      // [126, 255] range are such a special ones (see Appendix E, "Exit
      // Codes With Special Meanings" in the Advanced Bash-Scripting Guide).
      //
      exit_comparison comparison;
      uint8_t code;
    };

    // command
    //
    // Assume it is not very common to (un)set more than a few environment
    // variables in the script.
    //
    struct environment_vars: small_vector<string, 4>
    {
      // Find a variable (un)set.
      //
      // Note that only the variable name is considered for both arguments. In
      // other words, passing a variable set as a first argument can result
      // with a variable unset being found and vice versa.
      //
      environment_vars::iterator
      find (const string&);

      // Add or overwrite an existing variable (un)set.
      //
      void
      add (string);
    };

    // @@ For better diagnostics we may want to store an individual location
    //    of each command in the pipeline (maybe we can share the file part
    //    somehow since a pipline cannot span multiple files).
    //
    struct command
    {
      // We use NULL initial as an indication that the path stored in recall
      // is a program name that still needs to be resolved into the builtin
      // function or the process path.
      //
      process_path program;

      strings            arguments;

      // These come from the env builtin.
      //
      optional<dir_path> cwd;
      environment_vars   variables;
      optional<duration> timeout;
      bool               timeout_success = false;

      optional<redirect> in;
      optional<redirect> out;
      optional<redirect> err;

      script::cleanups cleanups;

      // If nullopt, then the command is expected to succeed (0 exit code).
      //
      optional<command_exit> exit;
    };

    enum class command_to_stream: uint16_t
    {
      header   = 0x01,
      here_doc = 0x02,              // Note: printed on a new line.
      all      = header | here_doc
    };

    void
    to_stream (ostream&, const command&, command_to_stream);

    ostream&
    operator<< (ostream&, const command&);

    // command_pipe
    //
    // Note that we cannot use small_vector here, since moving from objects of
    // the command_pipe type would invalidate the command redirects of the
    // reference type in this case.
    //
    using command_pipe = vector<command>;

    void
    to_stream (ostream&, const command_pipe&, command_to_stream);

    ostream&
    operator<< (ostream&, const command_pipe&);

    // command_expr
    //
    enum class expr_operator {log_or, log_and};

    struct expr_term
    {
      expr_operator op;  // OR-ed to an implied false for the first term.
      command_pipe pipe;
    };

    using command_expr = small_vector<expr_term, 1>;

    void
    to_stream (ostream&, const command_expr&, command_to_stream);

    ostream&
    operator<< (ostream&, const command_expr&);

    // Stack-allocated linked list of iteration indexes of the nested loops.
    //
    struct iteration_index
    {
      size_t index; // 1-based.

      const iteration_index* prev; // NULL for the top-most loop.
    };

    struct timeout
    {
      duration value;
      bool success;

      timeout (duration d, bool s): value (d), success (s) {}
    };

    struct deadline
    {
      timestamp value;
      bool success;

      deadline (timestamp t, bool s): value (t), success (s) {}
    };

    // If timestamps/durations are equal, the failure is less than the
    // success.
    //
    bool
    operator< (const deadline&, const deadline&);

    bool
    operator< (const timeout&, const timeout&);

    optional<deadline>
    to_deadline (const optional<timestamp>&, bool success);

    optional<timeout>
    to_timeout (const optional<duration>&, bool success);

    // Script execution environment.
    //
    class environment
    {
    public:
      build2::context& context;

      // The platform script programs run on.
      //
      const target_triplet& host;

      // The work directory is used as the builtin/process CWD and to complete
      // relative paths. Any attempt to remove or move this directory (or its
      // parent directory) using the rm or mv builtins will fail. Must be an
      // absolute path.
      //
      const dir_name_view work_dir;

      // If the sanbox directory is not NULL, then any attempt to remove or
      // move a filesystem entry outside this directory using an explicit
      // cleanup or the rm/mv builtins will fail, unless the --force option is
      // specified for the builtin. Must be an absolute path.
      //
      const dir_name_view sandbox_dir;

      // The temporary directory is used by the script running machinery to
      // create special files. Must be an absolute path, unless empty. Can be
      // empty until the create_temp_dir() function call, which can be used
      // for creating this directory on demand.
      //
      const dir_path& temp_dir;

      // If true, the temporary directory will not be removed on the script
      // failure. In particular, this allows the script running machinery to
      // refer to the special files in diagnostics.
      //
      const bool temp_dir_keep;

      // Default process streams redirects.
      //
      // If a stream redirect is not specified on the script command line,
      // then the respective redirect data member will be used as the default.
      //
      const redirect in;
      const redirect out;
      const redirect err;

      environment (build2::context& ctx,
                   const target_triplet& h,
                   const dir_name_view& wd,
                   const dir_name_view& sd,
                   const dir_path& td, bool tk,
                   redirect&& i = redirect (redirect_type::pass),
                   redirect&& o = redirect (redirect_type::pass),
                   redirect&& e = redirect (redirect_type::pass))
          : context (ctx), host (h),
            work_dir (wd), sandbox_dir (sd), temp_dir (td), temp_dir_keep (tk),
            in (move (i)), out (move (o)), err (move (e))
      {
      }

      // Create environment without the sandbox.
      //
      environment (build2::context& ctx,
                   const target_triplet& h,
                   const dir_name_view& wd,
                   const dir_path& td, bool tk,
                   redirect&& i = redirect (redirect_type::pass),
                   redirect&& o = redirect (redirect_type::pass),
                   redirect&& e = redirect (redirect_type::pass))
          : environment (ctx, h,
                         wd, dir_name_view (), td, tk,
                         move (i), move (o), move (e))
      {
      }

      // Cleanup.
      //
    public:
      script::cleanups cleanups;
      paths special_cleanups;

      // Register a cleanup. If the cleanup is explicit, then override the
      // cleanup type if this path is already registered. Ignore implicit
      // registration of a path outside sandbox directory, if specified (see
      // above).
      //
      void
      clean (cleanup, bool implicit);

      // Register cleanup of a special file. Such files are created to
      // maintain the script running machinery and must be removed first, not
      // to interfere with the user-defined wildcard cleanups if the working
      // and temporary directories are the same.
      //
      void
      clean_special (path);

      // Command execution environment variables.
      //
    public:
      // Environment variable (un)sets from the export builtin call.
      //
      // Each variable in the list can only be present once.
      //
      environment_vars exported_vars;

      // Return the environment variable (un)sets which can potentially rely
      // on factors besides the export builtin call sequence (scoping,
      // etc). The default implementation returns exported_vars.
      //
      virtual const environment_vars&
      exported_variables (environment_vars& storage);

      // Merge the own environment variable (un)sets with the specified ones,
      // overriding the former with the latter.
      //
      const environment_vars&
      merge_exported_variables (const environment_vars&,
                                environment_vars& storage);

    public:
      // Set variable value with optional (non-empty) attributes.
      //
      virtual void
      set_variable (string name,
                    names&&,
                    const string& attrs,
                    const location&) = 0;

      // Set the script execution timeout from the timeout builtin call.
      //
      // The builtin argument semantics is script implementation-dependent. If
      // success is true then a process missing this deadline should not be
      // considered as failed unless it didn't terminate gracefully and had to
      // be killed.
      //
      virtual void
      set_timeout (const string& arg, bool success, const location&) = 0;

      // Return the script execution deadline which can potentially rely on
      // factors besides the latest timeout builtin call (variables, scoping,
      // etc).
      //
      virtual optional<deadline>
      effective_deadline () = 0;

      // Create the temporary directory and set the temp_dir reference target
      // to its path. Must only be called if temp_dir is empty.
      //
      virtual void
      create_temp_dir () = 0;

    public:
      virtual
      ~environment () = default;
    };

    // Custom command function that can be executed at the end of the
    // pipeline. Should throw io_error on the underlying OS error.
    //
    // Note: the pipeline can be NULL (think of `for x <<<='foo'`).
    //
    struct pipe_command;

    using command_function = void (environment&,
                                   const strings& args,
                                   auto_fd in,
                                   pipe_command* pipeline,
                                   const optional<deadline>&,
                                   const location&);

    // Helpers.
    //
    // Issue diagnostics with the specified prefix and fail if the string
    // (potentially an option value) is not a valid variable name or
    // assignment (empty, etc).
    //
    void
    verify_environment_var_name (const string&,
                                 const char* prefix,
                                 const location&,
                                 const char* opt = nullptr);

    void
    verify_environment_var_assignment (const string&,
                                       const char* prefix,
                                       const location&);

    // "Unhide" operator<< from the build2 namespace.
    //
    using build2::operator<<;
  }
}

#include <libbuild2/script/script.ixx>

#endif // LIBBUILD2_SCRIPT_SCRIPT_HXX
