// file      : libbuild2/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_UTILITY_HXX
#define LIBBUILD2_UTILITY_HXX

#include <tuple>      // make_tuple()
#include <memory>     // make_shared()
#include <string>     // to_string()
#include <utility>    // move(), forward(), declval(), make_pair(), swap()
#include <cassert>    // assert()
#include <iterator>   // make_move_iterator()
#include <algorithm>  // *
#include <functional> // ref(), cref()

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
  using std::to_string;
  using std::stoul;
  using std::stoull;

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
             bool no_lines = false,
             bool no_columns = false,
             bool stderr_term = false);

  const uint16_t verb_never = 7;
  LIBBUILD2_SYMEXPORT extern uint16_t verb;
  LIBBUILD2_SYMEXPORT extern bool silent;

  // --[no-]progress
  //
  LIBBUILD2_SYMEXPORT extern optional<bool> diag_progress_option;

  LIBBUILD2_SYMEXPORT extern bool diag_no_line;   // --no-line
  LIBBUILD2_SYMEXPORT extern bool diag_no_column; // --no-column

  LIBBUILD2_SYMEXPORT extern bool stderr_term; // True if stderr is a terminal.

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

  // Whether running installed build and, if so, the library installation
  // directory (empty otherwise).
  //
  LIBBUILD2_SYMEXPORT extern const bool build_installed;
  LIBBUILD2_SYMEXPORT extern const dir_path build_install_lib; // $install.lib

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
  // The run*() functions with process_path assume that you are printing
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

  // Wait for process termination returning true if the process exited
  // normally with a zero code and false otherwise. The latter case is
  // normally followed up with a call to run_finish().
  //
  LIBBUILD2_SYMEXPORT bool
  run_wait (const char* args[], process&, const location& = location ());

  bool
  run_wait (cstrings& args, process&, const location& = location ());

  // Wait for process termination. Issue diagnostics and throw failed in case
  // of abnormal termination. If the process has terminated normally but with
  // a non-zero exit status, then assume the diagnostics has already been
  // issued and just throw failed. The last argument is used in cooperation
  // with run_start() in case STDERR is redirected to STDOUT.
  //
  void
  run_finish (const char* args[],
              process&,
              const string& = string (),
              const location& = location ());

  void
  run_finish (cstrings& args, process& pr, const location& l = location ());

  // As above but if the process has exited normally with a non-zero code,
  // then return false rather than throwing.
  //
  bool
  run_finish_code (const char* args[],
                   process&,
                   const string& = string (),
                   const location& = location ());

  // Start a process with the specified arguments. If in is -1, then redirect
  // STDIN to a pipe (can also be -2 to redirect to /dev/null or equivalent).
  // If out is -1, redirect STDOUT to a pipe. If error is false, then
  // redirecting STDERR to STDOUT (this can be used to suppress diagnostics
  // from the child process). Issue diagnostics and throw failed in case of an
  // error.
  //
  LIBBUILD2_SYMEXPORT process
  run_start (uint16_t verbosity,
             const process_env&, // Implicit-constructible from process_path.
             const char* args[],
             int in = 0,
             int out = 1,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const location& = location ());

  inline process
  run_start (uint16_t verbosity,
             const process_env& pe,
             cstrings& args,
             int in = 0,
             int out = 1,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const location& l = location ())
  {
    return run_start (verbosity, pe, args.data (), in, out, error, cwd, l);
  }

  inline process
  run_start (const process_env& pe,
             const char* args[],
             int in = 0,
             int out = 1,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const location& l = location ())
  {
    return run_start (verb_never, pe, args, in, out, error, cwd, l);
  }

  inline process
  run_start (const process_env& pe,
             cstrings& args,
             int in = 0,
             int out = 1,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const location& l = location ())
  {
    return run_start (pe, args.data (), in, out, error, cwd, l);
  }

  inline void
  run (const process_env& pe, // Implicit-constructible from process_path.
       const char* args[])
  {
    process pr (run_start (pe, args));
    run_finish (args, pr);
  }

  inline void
  run (const process_env& pe,  // Implicit-constructible from process_path.
       cstrings& args)
  {
    run (pe, args.data ());
  }

  inline void
  run (const process_path& p,
       const char* args[],
       const dir_path& cwd,
       const char* const* env = nullptr)
  {
    process pr (run_start (process_env (p, env), args, 0, 1, true, cwd));
    run_finish (args, pr);
  }

  inline void
  run (const process_path& p,
       cstrings& args,
       const dir_path& cwd,
       const char* const* env = nullptr)
  {
    run (p, args.data (), cwd, env);
  }

  // As above, but search for the process (including updating args[0]) and
  // print the process commands line at the specified verbosity level.
  //
  inline process
  run_start (uint16_t verbosity,
             const char* args[],
             int in = 0,
             int out = 1,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const char* const* env = nullptr,
             const location& l = location ())
  {
    process_path pp (run_search (args[0], l));
    return run_start (verbosity,
                      process_env (pp, env), args,
                      in, out, error,
                      cwd, l);
  }

  inline process
  run_start (uint16_t verbosity,
             cstrings& args,
             int in = 0,
             int out = 1,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const char* const* env = nullptr,
             const location& l = location ())
  {
    return run_start (verbosity, args.data (), in, out, error, cwd, env, l);
  }

  inline void
  run (uint16_t verbosity,
       const char* args[],
       const dir_path& cwd = dir_path (),
       const char* const* env = nullptr)
  {
    process pr (run_start (verbosity, args, 0, 1, true, cwd, env));
    run_finish (args, pr);
  }

  inline void
  run (uint16_t verbosity,
       cstrings& args,
       const dir_path& cwd = dir_path (),
       const char* const* env = nullptr)
  {
    run (verbosity, args.data (), cwd, env);
  }

  // Start the process as above and then call the specified function on each
  // trimmed line of the output until it returns a non-empty object T (tested
  // with T::empty()) which is then returned to the caller.
  //
  // The predicate can move the value out of the passed string but, if error
  // is false, only in case of a "content match" (so that any diagnostics
  // lines are left intact). The function signature should be:
  //
  // T (string& line, bool last)
  //
  // If ignore_exit is true, then the program's exit status is ignored (if it
  // is false and the program exits with the non-zero status, then an empty T
  // instance is returned).
  //
  // If checksum is not NULL, then feed it the content of each trimmed line
  // (including those that come after the callback returns non-empty object).
  //
  template <typename T, typename F>
  T
  run (uint16_t verbosity,
       const process_env&, // Implicit-constructible from process_path.
       const char* args[],
       F&&,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr);

  template <typename T, typename F>
  inline T
  run (const process_env& pe, // Implicit-constructible from process_path.
       const char* args[],
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    return run<T> (
      verb_never, pe, args, forward<F> (f), error, ignore_exit, checksum);
  }

  template <typename T, typename F>
  inline T
  run (uint16_t verbosity,
       const char* args[],
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    process_path pp (run_search (args[0]));
    return run<T> (
      verbosity, pp, args, forward<F> (f), error, ignore_exit, checksum);
  }

  // run <prog>
  //
  template <typename T, typename F>
  inline T
  run (uint16_t verbosity,
       const path& prog,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    const char* args[] = {prog.string ().c_str (), nullptr};
    return run<T> (
      verbosity, args, forward<F> (f), error, ignore_exit, checksum);
  }

  template <typename T, typename F>
  inline T
  run (uint16_t verbosity,
       const process_env& pe, // Implicit-constructible from process_path.
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    const char* args[] = {pe.path->recall_string (), nullptr};
    return run<T> (
      verbosity, pe, args, forward<F> (f), error, ignore_exit, checksum);
  }

  // run <prog> <arg>
  //
  template <typename T, typename F>
  inline T
  run (uint16_t verbosity,
       const path& prog,
       const char* arg,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    const char* args[] = {prog.string ().c_str (), arg, nullptr};
    return run<T> (
      verbosity, args, forward<F> (f), error, ignore_exit, checksum);
  }

  template <typename T, typename F>
  inline T
  run (uint16_t verbosity,
       const process_env& pe, // Implicit-constructible from process_path.
       const char* arg,
       F&& f,
       bool error = true,
       bool ignore_exit = false,
       sha256* checksum = nullptr)
  {
    const char* args[] = {pe.path->recall_string (), arg, nullptr};
    return run<T> (
      verbosity, pe, args, forward<F> (f), error, ignore_exit, checksum);
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
  append_option_values (cstrings&,
                        const char* opt,
                        I begin, I end,
                        F&& get = [] (const string& s) {return s.c_str ();});

  template <typename I, typename F>
  void
  append_option_values (sha256&,
                        const char* opt,
                        I begin, I end,
                        F&& get = [] (const string& s) {return s;});

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
