// file      : libbuild2/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_UTILITY_HXX
#define LIBBUILD2_UTILITY_HXX

#include <tuple>       // make_tuple()
#include <memory>      // make_shared()
#include <string>      // to_string()
#include <utility>     // move(), forward(), declval(), make_pair(), swap()
#include <cassert>     // assert()
#include <iterator>    // make_move_iterator(), back_inserter()
#include <algorithm>   // *
#include <functional>  // ref(), cref()
#include <type_traits>

#include <libbutl/ft/lang.hxx>

#include <libbutl/utility.hxx>  // combine_hash(), reverse_iterate(), etc
#include <libbutl/fdstream.hxx>
#include <libbutl/path-pattern.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>

// "Fake" version values used during bootstrap.
//
#ifdef BUILD2_BOOTSTRAP
#  define LIBBUILD2_VERSION     9999999999999990000ULL
#  define LIBBUILD2_VERSION_STR "99999.99999.99999"
#  define LIBBUILD2_VERSION_ID  "99999.99999.99999"
#  define LIBBUTL_VERSION_STR   "99999.99999.99999"
#  define LIBBUTL_VERSION_ID    "99999.99999.99999"
#else
#  include <libbuild2/version.hxx>
#endif

#include <libbuild2/export.hxx>

namespace build2
{
  using std::move;
  using std::swap;
  using std::forward;
  using std::declval;

  using std::ref;
  using std::cref;

  using std::make_pair;
  using std::make_tuple;
  using std::make_shared;
  using std::make_move_iterator;
  using std::back_inserter;
  using std::stoul;
  using std::stoull;

  using std::to_string;

  // Currently only supports base 10 and 16. Note: adds `0x` if base 16.
  //
  LIBBUILD2_SYMEXPORT string
  to_string (uint64_t, int base, size_t width = 0);

  // <libbutl/utility.hxx>
  //
  using butl::reverse_iterate;
  using butl::compare_c_string;
  using butl::compare_pointer_target;
  //using butl::hash_pointer_target;
  using butl::combine_hash;
  using butl::icasecmp;
  using butl::icase_compare_string;
  using butl::icase_compare_c_string;
  using butl::lcase;
  using butl::ucase;
  using butl::alpha;
  using butl::alnum;
  using butl::digit;
  using butl::wspace;

  using butl::trim;
  using butl::next_word;
  using butl::sanitize_identifier;
  using butl::sanitize_strlit;

  using butl::make_guard;
  using butl::make_exception_guard;

  using butl::function_cast;

  using butl::getenv;
  using butl::auto_thread_env;

  using butl::throw_generic_error;
  using butl::throw_system_error;

  using butl::eof;

  // <libbutl/fdstream.hxx>
  //
  using butl::fdopen_null;
  using butl::open_file_or_stdin;
  using butl::open_file_or_stdout;

  // <libbutl/path-pattern.hxx>
  //
  using butl::path_pattern;
  using butl::path_match;

  // Perform process-wide initializations/adjustments/workarounds. Should be
  // called once early in main(). In particular, besides other things, this
  // functions does the following:
  //
  // - Sets PATH to include baseutils /bin on Windows.
  //
  // - Ignores SIGPIPE.
  //
  // - Calls tzset().
  //
  LIBBUILD2_SYMEXPORT void
  init_process ();

  // Diagnostics state (verbosity level, etc; see <libbuild2/diagnostics.hxx>).
  //
  // Note on naming of values (here and in the global state below) that come
  // from the command line options: if a value is not meant to be used
  // directly, then it has the _option suffix and a function or another
  // variable as its public interface.

  // Initialize the diagnostics state. Should be called once early in main().
  // Default values are for unit tests.
  //
  // If silent is true, verbosity should be 0.
  //
  LIBBUILD2_SYMEXPORT void
  init_diag (uint16_t verbosity,
             bool silent = false,
             optional<bool> progress = nullopt,
             optional<bool> diag_color = nullopt,
             bool no_lines = false,
             bool no_columns = false,
             bool stderr_term = false);

  const uint16_t verb_never = 7;
  LIBBUILD2_SYMEXPORT extern uint16_t verb;
  LIBBUILD2_SYMEXPORT extern bool silent;

  // --[no-]progress
  // --[no-]diag-color
  //
  LIBBUILD2_SYMEXPORT extern optional<bool> diag_progress_option;
  LIBBUILD2_SYMEXPORT extern optional<bool> diag_color_option;

  LIBBUILD2_SYMEXPORT extern bool diag_no_line;   // --no-line
  LIBBUILD2_SYMEXPORT extern bool diag_no_column; // --no-column

  // True if stderr is a terminal.
  //
  LIBBUILD2_SYMEXPORT extern bool stderr_term;

  // True if the color can be used on the stderr terminal.
  //
  LIBBUILD2_SYMEXPORT extern bool stderr_term_color;

  // Global state (verbosity, home/work directories, etc).

  // Initialize the global state. Should be called once early in main().
  // Default values are for unit tests.
  //
  LIBBUILD2_SYMEXPORT void
  init (void (*terminate) (bool),
        const char* argv0,
        bool serial_stop,
        optional<bool> mtime_check = nullopt,
        optional<path> config_sub = nullopt,
        optional<path> config_guess = nullopt);

  // Terminate function. If trace is false, then printing of the stack trace,
  // if any, should be omitted.
  //
  LIBBUILD2_SYMEXPORT extern void (*terminate) (bool trace);

  // Build system driver process path (argv0.initial is argv[0]).
  //
  LIBBUILD2_SYMEXPORT extern process_path argv0;

  // Build system core version and interface version.
  //
  LIBBUILD2_SYMEXPORT extern const standard_version build_version;
  LIBBUILD2_SYMEXPORT extern const string build_version_interface;

  // Whether running installed build as well as the library installation
  // directory (only if installed, empty otherwise), the exported buildfile
  // installation directory (only if configured, empty otherwise), and data
  // installation directory (only if installed, src_root otherwise).
  //
  LIBBUILD2_SYMEXPORT extern const bool build_installed;
  LIBBUILD2_SYMEXPORT extern const dir_path build_install_lib; // $install.lib
  LIBBUILD2_SYMEXPORT extern const dir_path build_install_buildfile; // $install.buildfile
  LIBBUILD2_SYMEXPORT extern const dir_path build_install_data; // $install.data

  // --[no-]mtime-check
  //
  LIBBUILD2_SYMEXPORT extern optional<bool> mtime_check_option;

  LIBBUILD2_SYMEXPORT extern optional<path> config_sub;   // --config-sub
  LIBBUILD2_SYMEXPORT extern optional<path> config_guess; // --config-guess

  LIBBUILD2_SYMEXPORT void
  check_build_version (const standard_version_constraint&, const location&);

  // Work/home directories (must be initialized in main()) and relative path
  // calculation.
  //
  LIBBUILD2_SYMEXPORT extern dir_path work;
  LIBBUILD2_SYMEXPORT extern dir_path home;

  // By default this points to work. Setting this to something else should
  // only be done in tightly controlled, non-concurrent situations (e.g.,
  // state dump). If it is empty, then relative() below returns the original
  // path.
  //
  // Note: watch out for concurrent changes from multiple build contexts.
  //
  LIBBUILD2_SYMEXPORT extern const dir_path* relative_base;

  // If possible and beneficial, translate an absolute, normalized path into
  // relative to the relative_base directory, which is normally work. Note
  // that if the passed path is the same as relative_base, then this function
  // returns empty path.
  //
  template <typename K>
  basic_path<char, K>
  relative (const basic_path<char, K>&);

  class path_target;

  LIBBUILD2_SYMEXPORT path
  relative (const path_target&);

  // In addition to calling relative(), this function also uses shorter
  // notations such as '~/'. For directories the result includes the trailing
  // slash. If the path is the same as base, returns "./" if current is true
  // and empty string otherwise.
  //
  LIBBUILD2_SYMEXPORT string
  diag_relative (const path&, bool current = true);

  // Basic process utilities.
  //
  // The run*() functions with process_path/_env assume that you are printing
  // the process command line yourself.

  // Search for a process executable. Issue diagnostics and throw failed in
  // case of an error.
  //
  LIBBUILD2_SYMEXPORT process_path
  run_search (const char*& args0,
              bool path_only,
              const location& = location ());

  inline process_path
  run_search (const char*& args0, const location& l = location ())
  {
    return run_search (args0, false, l);
  }

  LIBBUILD2_SYMEXPORT process_path
  run_search (const path&,
              bool init = false,
              const dir_path& fallback = dir_path (),
              bool path_only = false,
              const location& = location ());

  LIBBUILD2_SYMEXPORT process_path
  run_try_search (const path&,
                  bool init = false,
                  const dir_path& fallback = dir_path (),
                  bool path_only = false,
                  const char* paths = nullptr);

  [[noreturn]] LIBBUILD2_SYMEXPORT void
  run_search_fail (const path&, const location& = location ());

  // Start a process with the specified arguments. Issue diagnostics and throw
  // failed in case of an error. If in is -1, then redirect stdin to a pipe
  // (can also be -2 to redirect it to /dev/null or equivalent). If out is -1,
  // then redirect stdout to a pipe. If stderr is redirected to stdout (can
  // be used to analyze diagnostics from the child process), then, in case of
  // an error, the last line read from stdout must be passed to run_finish()
  // below.
  //
  LIBBUILD2_SYMEXPORT process
  run_start (uint16_t verbosity,
             const process_env&, // Implicit-constructible from process_path.
             const char* const* args,
             int in = 0,
             int out = 1,
             int err = 2,
             const location& = {});

  inline process
  run_start (uint16_t verbosity,
             const process_env& pe,
             const cstrings& args,
             int in = 0,
             int out = 1,
             int err = 2,
             const location& l = {})
  {
    return run_start (verbosity, pe, args.data (), in, out, err, l);
  }

  inline process
  run_start (const process_env& pe,
             const char* const* args,
             int in = 0,
             int out = 1,
             int err = 2,
             const location& l = {})
  {
    return run_start (verb_never, pe, args, in, out, err, l);
  }

  inline process
  run_start (const process_env& pe,
             const cstrings& args,
             int in = 0,
             int out = 1,
             int err = 2,
             const location& l = {})
  {
    return run_start (pe, args.data (), in, out, err, l);
  }

  // As above, but search for the process (including updating args[0]) and
  // print the process commands line at the specified verbosity level.
  //
  inline process
  run_start (uint16_t verbosity,
             const char* args[],
             int in = 0,
             int out = 1,
             int err = 2,
             const char* const* env = nullptr,
             const dir_path& cwd = {},
             const location& l = {})
  {
    process_path pp (run_search (args[0], l));
    return run_start (verbosity,
                      process_env (pp, cwd, env), args,
                      in, out, err,
                      l);
  }

  inline process
  run_start (uint16_t verbosity,
             cstrings& args,
             int in = 0,
             int out = 1,
             int err = 2,
             const char* const* env = nullptr,
             const dir_path& cwd = {},
             const location& l = {})
  {
    return run_start (verbosity, args.data (), in, out, err, env, cwd, l);
  }

  // Wait for process termination returning true if the process exited
  // normally with a zero code and false otherwise. The latter case is
  // normally followed up with a call to run_finish().
  //
  LIBBUILD2_SYMEXPORT bool
  run_wait (const char* const* args, process&, const location& = location ());

  bool
  run_wait (const cstrings& args, process&, const location& = location ());

  // Wait for process termination, issues diagnostics, and throw failed.
  //
  // If the child process exited abnormally or normally with non-0 code, then
  // print the error diagnostics to this effect. Additionally, if the
  // verbosity level is between 1 and the specified value, then print the
  // command line as info after the error. If omit_normal is true, then don't
  // print either for the normal exit (usually used for custom diagnostics or
  // when process failure can be tolerated).
  //
  // Normally the specified verbosity will be 1 and the command line args
  // represent the verbosity level 2 (logical) command line. Or, to put it
  // another way, it should be 1 less than what gets passed to run_start().
  // Note that args should only represent a single command in a pipe (see
  // print_process() for details).
  //
  // See also diag_buffer::close().
  //
  // The line argument is used in cooperation with run_start() to diagnose a
  // failure to exec in case stderr is redirected to stdout (see the
  // implementation for details).
  //
  void
  run_finish (const char* const* args,
              process&,
              uint16_t verbosity,
              bool omit_normal = false,
              const location& = location ());

  void
  run_finish (const cstrings& args,
              process&,
              uint16_t verbosity,
              bool omit_normal = false,
              const location& = location ());

  void
  run_finish (const char* const* args,
              process&,
              const string& line,
              uint16_t verbosity,
              bool omit_normal = false,
              const location& = location ());

  // As above but if the process has exited normally with a non-zero code,
  // then return false rather than throwing.
  //
  // Note that the normal non-0 exit diagnostics is omitted by default
  // assuming appropriate custom diagnostics will be issued, if required.
  //
  bool
  run_finish_code (const char* const* args,
                   process&,
                   uint16_t verbosity,
                   bool omit_normal = true,
                   const location& = location ());

  bool
  run_finish_code (const cstrings& args,
                   process&,
                   uint16_t verbosity,
                   bool omit_normal = true,
                   const location& = location ());

  bool
  run_finish_code (const char* const* args,
                   process&,
                   const string&,
                   uint16_t verbosity,
                   bool omit_normal = true,
                   const location& = location ());

  // As above but with diagnostics buffering.
  //
  // Specifically, this version first waits for the process termination, then
  // calls diag_buffer::close(verbosity, omit_normal), and finally throws
  // failed if the process didn't exit with 0 code.
  //
  class diag_buffer;

  void
  run_finish (diag_buffer&,
              const char* const* args,
              process&,
              uint16_t verbosity,
              bool omit_normal = false,
              const location& = location ());

  void
  run_finish (diag_buffer&,
              const cstrings& args,
              process&,
              uint16_t verbosity,
              bool omit_normal = false,
              const location& = location ());

  // As above but if the process has exited normally with a non-zero code,
  // then return false rather than throwing.
  //
  // Note that the normal non-0 exit diagnostics is omitted by default
  // assuming appropriate custom diagnostics will be issued, if required.
  //
  bool
  run_finish_code (diag_buffer&,
                   const char* const* args,
                   process&,
                   uint16_t verbosity,
                   bool omit_normal = true,
                   const location& = location ());

  bool
  run_finish_code (diag_buffer&,
                   const cstrings& args,
                   process&,
                   uint16_t verbosity,
                   bool omit_normal = true,
                   const location& = location ());

  // Run the process with the specified arguments by calling the above start
  // and finish functions. Buffer diagnostics unless in the load phase.
  //
  LIBBUILD2_SYMEXPORT void
  run (context&,
       const process_env& pe, // Implicit-constructible from process_path.
       const char* const* args,
       uint16_t finish_verbosity);

  inline void
  run (context& ctx,
       const process_env& pe,
       const cstrings& args,
       uint16_t finish_verbosity)
  {
    run (ctx, pe, args.data (), finish_verbosity);
  }

  // As above but pass cwd/env vars as arguments rather than as part of
  // process_env.
  //
  inline void
  run (context& ctx,
       const process_path& p,
       const char* const* args,
       uint16_t finish_verbosity,
       const char* const* env,
       const dir_path& cwd = {})
  {
    run (ctx, process_env (p, cwd, env), args, finish_verbosity);
  }

  inline void
  run (context& ctx,
       const process_path& p,
       const cstrings& args,
       uint16_t finish_verbosity,
       const char* const* env,
       const dir_path& cwd = {})
  {
    run (ctx, p, args.data (), finish_verbosity, env, cwd);
  }

  // Start the process as above and then call the specified function on each
  // trimmed line of the output until it returns a non-empty object T (tested
  // with T::empty()) which is then returned to the caller.
  //
  // If verbosity is specified, print the process commands line at that level
  // (with the verbosite-1 value passed run_finish()).
  //
  // If error is false, then redirecting stderr to stdout and don't fail if
  // the process exits normally but with non-0 code (can be used to suppress
  // and/or analyze diagnostics from the child process). Otherwise, buffer
  // diagnostics unless in the load phase.
  //
  // The predicate can move the value out of the passed string but, if error
  // is false, only in case of a "content match" (so that any diagnostics
  // lines are left intact). The function signature should be:
  //
  // T (string& line, bool last)
  //
  // If, in addition to error being false, ignore_exit is true, then the
  // program's normal exit status is ignored (if it is false and the program
  // exits with the non-zero status, then an empty T instance is returned).
  //
  // If checksum is not NULL, then feed it the content of each trimmed line
  // (including those that come after the callback returns non-empty object).
  //
  template <typename T, typename F>
  T
  run (context&,
       uint16_t verbosity,
       const process_env&, // Implicit-constructible from process_path.
       const char* const* args,
       F&&,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr);

  template <typename T, typename F>
  inline T
  run (context& ctx,
       uint16_t verbosity,
       const process_env& pe,
       const cstrings& args,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    return run<T> (ctx,
                   verbosity,
                   pe, args.data (),
                   forward<F> (f),
                   error, ignore_exit, checksum);
  }

  template <typename T, typename F>
  inline T
  run (context&,
       const process_env&,
       const char* const* args,
       uint16_t finish_verbosity,
       F&&,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr);

  template <typename T, typename F>
  inline T
  run (context& ctx,
       const process_env& pe,
       const cstrings& args,
       uint16_t finish_verbosity,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    return run<T> (ctx,
                   pe, args.data (),
                   finish_verbosity,
                   forward<F> (f),
                   error, ignore_exit, checksum);
  }

  template <typename T, typename F>
  inline T
  run (context& ctx,
       uint16_t verbosity,
       const char* args[],
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    process_path pp (run_search (args[0]));
    return run<T> (ctx,
                   verbosity,
                   pp, args,
                   forward<F> (f),
                   error, ignore_exit, checksum);
  }

  template <typename T, typename F>
  inline T
  run (context& ctx,
       uint16_t verbosity,
       cstrings& args,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    return run<T> (ctx,
                   verbosity,
                   args.data (),
                   forward<F> (f),
                   error, ignore_exit, checksum);
  }

  // As above but run a program without any arguments or with one argument.
  //
  // run <prog>
  //
  template <typename T, typename F>
  inline T
  run (context& ctx,
       uint16_t verbosity,
       const path& prog,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    const char* args[] = {prog.string ().c_str (), nullptr};
    return run<T> (ctx,
                   verbosity,
                   args,
                   forward<F> (f),
                   error, ignore_exit, checksum);
  }

  template <typename T, typename F>
  inline typename std::enable_if<
    (!std::is_same<typename std::decay<F>::type, const char**>::value &&
     !std::is_same<typename std::remove_reference<F>::type, cstrings>::value),
    T>::type
  run (context& ctx,
       uint16_t verbosity,
       const process_env& pe, // Implicit-constructible from process_path.
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    const char* args[] = {pe.path->recall_string (), nullptr};
    return run<T> (ctx,
                   verbosity,
                   pe, args,
                   forward<F> (f),
                   error, ignore_exit, checksum);
  }

  // run <prog> <arg>
  //
  template <typename T, typename F>
  inline T
  run (context& ctx,
       uint16_t verbosity,
       const path& prog,
       const char* arg,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    const char* args[] = {prog.string ().c_str (), arg, nullptr};
    return run<T> (ctx,
                   verbosity,
                   args,
                   forward<F> (f),
                   error, ignore_exit, checksum);
  }

  template <typename T, typename F>
  inline T
  run (context& ctx,
       uint16_t verbosity,
       const process_env& pe, // Implicit-constructible from process_path.
       const char* arg,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    const char* args[] = {pe.path->recall_string (), arg, nullptr};
    return run<T> (ctx,
                   verbosity,
                   pe, args,
                   forward<F> (f),
                   error, ignore_exit, checksum);
  }

  // As above but a lower-level interface that erases T and F and can also be
  // used to suppress trimming.
  //
  // The passed function should return true if it should be called again
  // (i.e., the object is still empty in the T & F interface) and false
  // otherwise.
  //
  // The first version ruturn true if the result is usable and false
  // otherwise, depending on the process exit code and error/ignore_exit
  // values. (In the latter case, the T & F interface makes the resulting
  // object empty).
  //
  LIBBUILD2_SYMEXPORT bool
  run (context&,
       uint16_t verbosity,
       const process_env&,
       const char* const* args,
       uint16_t finish_verbosity,
       const function<bool (string& line, bool last)>&,
       bool trim = true,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr);

  // Concatenate the program path and arguments into a shallow NULL-terminated
  // vector of C-strings.
  //
  LIBBUILD2_SYMEXPORT cstrings
  process_args (const char* program, const strings& args);

  inline cstrings
  process_args (const string& program, const strings& args)
  {
    return process_args (program.c_str (), args);
  }

  // File descriptor streams.
  //
  fdpipe
  open_pipe ();

  auto_fd
  open_null ();

  // Empty/nullopt string, path, and project name.
  //
  LIBBUILD2_SYMEXPORT extern const string       empty_string;
  LIBBUILD2_SYMEXPORT extern const path         empty_path;
  LIBBUILD2_SYMEXPORT extern const dir_path     empty_dir_path;
  LIBBUILD2_SYMEXPORT extern const project_name empty_project_name;

  LIBBUILD2_SYMEXPORT extern const optional<string>       nullopt_string;
  LIBBUILD2_SYMEXPORT extern const optional<path>         nullopt_path;
  LIBBUILD2_SYMEXPORT extern const optional<dir_path>     nullopt_dir_path;
  LIBBUILD2_SYMEXPORT extern const optional<project_name> nullopt_project_name;

  // Hash a path potentially without the specific directory prefix.
  //
  // If prefix is not empty and is a super-path of the path to hash, then only
  // hash the suffix. Note that both paths are assumed to be normalized.
  //
  // This functionality is normally used to strip out_root from target paths
  // being hashed in order to avoid updates in case out_root was moved. Note
  // that this should only be done if the result of the update does not
  // include the out_root path in any form (as could be the case, for example,
  // for debug information, __FILE__ macro expansion, rpath, etc).
  //
  void
  hash_path (sha256&, const path&, const dir_path& prefix = dir_path ());

  // Append all the values from a variable to the C-string list. T is either
  // target or scope. The variable is expected to be of type strings.
  //
  // If excl is not NULL, then filter this option out (note: case sensitive).
  //
  template <typename T>
  void
  append_options (cstrings&, T&, const variable&, const char* excl = nullptr);

  template <typename T>
  void
  append_options (cstrings&, T&, const char*, const char* excl = nullptr);

  template <typename T>
  void
  append_options (strings&, T&, const variable&, const char* excl = nullptr);

  template <typename T>
  void
  append_options (strings&, T&, const char*, const char* excl = nullptr);

  template <typename T>
  void
  append_options (sha256&, T&, const variable&);

  template <typename T>
  void
  append_options (sha256&, T&, const char*);

  // As above but from the lookup directly.
  //
  LIBBUILD2_SYMEXPORT void
  append_options (cstrings&, const lookup&, const char* excl = nullptr);

  LIBBUILD2_SYMEXPORT void
  append_options (strings&, const lookup&, const char* excl = nullptr);

  LIBBUILD2_SYMEXPORT void
  append_options (sha256&, const lookup&);

  // As above but from the strings value directly.
  //
  void
  append_options (cstrings&, const strings&, const char* excl = nullptr);

  void
  append_options (strings&, const strings&, const char* excl = nullptr);

  void
  append_options (sha256&, const strings&);

  LIBBUILD2_SYMEXPORT void
  append_options (cstrings&,
                  const strings&, size_t,
                  const char* excl = nullptr);

  LIBBUILD2_SYMEXPORT void
  append_options (strings&,
                  const strings&, size_t,
                  const char* excl = nullptr);

  LIBBUILD2_SYMEXPORT void
  append_options (sha256&, const strings&, size_t);

  // As above but append/hash option values for the specified option (e.g.,
  // -I, -L).
  //
  template <typename I, typename F>
  void
  append_option_values (
    cstrings&,
    const char* opt,
    I begin, I end,
    F&& get = [] (const string& s) {return s.c_str ();});

  template <typename I, typename F>
  void
  append_option_values (
    sha256&,
    const char* opt,
    I begin, I end,
    F&& get = [] (const string& s) -> const string& {return s;});

  // As above but in a combined form (e.g., -L/usr/local/lib).
  //
  template <typename I, typename F>
  void
  append_combined_option_values (
    strings&,
    const char* opt,
    I begin, I end,
    F&& get = [] (const string& s) -> const string& {return s;});

  // As above but append a single option (used for append/hash uniformity).
  //
  inline void
  append_option (cstrings& args, const char* o)
  {
    args.push_back (o);
  }

  inline void
  append_option (strings& args, const char* o)
  {
    args.push_back (o);
  }

  inline void
  append_option (sha256& csum, const char* o)
  {
    csum.append (o);
  }

  // Check if a specified option is present in the variable or value. T is
  // either target or scope. For the interator version use rbegin()/rend() to
  // search backwards.
  //
  template <typename T>
  bool
  find_option (const char* option,
               T&,
               const variable&,
               bool ignore_case = false);

  template <typename T>
  bool
  find_option (const char* option,
               T&,
               const char* variable,
               bool ignore_case = false);

  template <typename I>
  I
  find_option (const char* option, I begin, I end, bool ignore_case = false);

  LIBBUILD2_SYMEXPORT bool
  find_option (const char* option, const lookup&, bool ignore_case = false);

  LIBBUILD2_SYMEXPORT bool
  find_option (const char* option, const strings&, bool ignore_case = false);

  LIBBUILD2_SYMEXPORT bool
  find_option (const char* option, const cstrings&, bool ignore_case = false);

  // As above but look for several options returning true if any is present.
  //
  template <typename T>
  bool
  find_options (const initializer_list<const char*>&,
                T&,
                const variable&,
                bool = false);

  template <typename T>
  bool
  find_options (const initializer_list<const char*>&,
                T&,
                const char*,
                bool = false);

  LIBBUILD2_SYMEXPORT bool
  find_options (const initializer_list<const char*>&,
                const lookup&,
                bool = false);

  LIBBUILD2_SYMEXPORT bool
  find_options (const initializer_list<const char*>&,
                const strings&,
                bool = false);

  LIBBUILD2_SYMEXPORT bool
  find_options (const initializer_list<const char*>&,
                const cstrings&,
                bool = false);

  // As above but look for an option that has the specified prefix. Return the
  // pointer to option or NULL if not found (thus can be used as bool).
  // Search backward (which is normally consistent with how options override
  // each other). For the interator version use rbegin()/rend() to do the
  // same.
  //
  template <typename T>
  const string*
  find_option_prefix (const char* prefix, T&, const variable&, bool = false);

  template <typename T>
  const string*
  find_option_prefix (const char* prefix, T&, const char*, bool = false);

  template <typename I>
  I
  find_option_prefix (const char* prefix, I begin, I end, bool = false);

  LIBBUILD2_SYMEXPORT const string*
  find_option_prefix (const char* prefix, const lookup&, bool = false);

  LIBBUILD2_SYMEXPORT const string*
  find_option_prefix (const char* prefix, const strings&, bool = false);

  LIBBUILD2_SYMEXPORT const char*
  find_option_prefix (const char* prefix, const cstrings&, bool = false);

  // As above but look for several option prefixes.
  //
  template <typename T>
  const string*
  find_option_prefixes (const initializer_list<const char*>&,
                        T&,
                        const variable&,
                        bool = false);

  template <typename T>
  const string*
  find_option_prefixes (const initializer_list<const char*>&,
                        T&,
                        const char*,
                        bool = false);

  LIBBUILD2_SYMEXPORT const string*
  find_option_prefixes (const initializer_list<const char*>&,
                        const lookup&, bool = false);

  LIBBUILD2_SYMEXPORT const string*
  find_option_prefixes (const initializer_list<const char*>&,
                        const strings&,
                        bool = false);

  LIBBUILD2_SYMEXPORT const char*
  find_option_prefixes (const initializer_list<const char*>&,
                        const cstrings&,
                        bool = false);

  // Hash environment variable (its name and value) normally to be used as a
  // checksum. See also config::save_environment().
  //
  void
  hash_environment (sha256&, const char* name);

  void
  hash_environment (sha256&, const string& name);

  void
  hash_environment (sha256&, initializer_list<const char*> names);

  string
  hash_environment (initializer_list<const char*> names);

  void
  hash_environment (sha256&, const cstrings& names);

  string
  hash_environment (const cstrings& names);

  void
  hash_environment (sha256&, const strings& names);

  string
  hash_environment (const strings& names);

  // A NULL-terminated list of variables (may itself be NULL).
  //
  void
  hash_environment (sha256&, const char* const* names);

  string
  hash_environment (const char* const* names);

  // Find in the string the stem separated from other characters with the
  // specified separators or begin/end of the string. Return the stem's
  // position or npos if not found.
  //
  size_t
  find_stem (const string&, size_t pos, size_t n,
             const char* stem, const char* seps = "-_.");

  inline size_t
  find_stem (const string& s, const char* stem, const char* seps = "-_.")
  {
    return find_stem (s, 0, s.size (), stem, seps);
  }

  // Apply the specified substitution (stem) to a '*'-pattern. If pattern is
  // NULL or empty, then return the stem itself. Assume the pattern is valid,
  // i.e., contains a single '*' character.
  //
  LIBBUILD2_SYMEXPORT string
  apply_pattern (const char* stem, const char* pattern);

  inline string
  apply_pattern (const char* s, const string* p)
  {
    return apply_pattern (s, p != nullptr ? p->c_str () : nullptr);
  }

  inline string
  apply_pattern (const char* s, const string& p)
  {
    return apply_pattern (s, p.c_str ());
  }

  // Try to parse a string as a non-negative number returning nullopt if the
  // argument is not a valid number or the number is greater than the
  // specified maximum.
  //
  optional<uint64_t>
  parse_number (const string&, uint64_t max = UINT64_MAX);
}

#include <libbuild2/utility.ixx>
#include <libbuild2/utility.txx>

#endif // LIBBUILD2_UTILITY_HXX
