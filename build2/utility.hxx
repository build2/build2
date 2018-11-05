// file      : build2/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_UTILITY_HXX
#define BUILD2_UTILITY_HXX

#include <tuple>      // make_tuple()
#include <memory>     // make_shared()
#include <string>     // to_string()
#include <utility>    // move(), forward(), declval(), make_pair()
#include <cassert>    // assert()
#include <iterator>   // make_move_iterator()
#include <algorithm>  // *
#include <functional> // ref(), cref()

#include <libbutl/ft/lang.hxx>

#include <libbutl/utility.mxx> // combine_hash(), reverse_iterate(), etc

#include <unordered_set>

#include <build2/types.hxx>
#include <build2/b-options.hxx>

// "Fake" version values used during bootstrap.
//
#ifdef BUILD2_BOOTSTRAP
#  define BUILD2_VERSION      9999999990000ULL
#  define BUILD2_VERSION_STR  "999.999.999"
#  define BUILD2_VERSION_ID   "999.999.999"
#  define LIBBUTL_VERSION_STR "999.999.999"
#  define LIBBUTL_VERSION_ID  "999.999.999"
#else
#  include <build2/version.hxx>
#endif

namespace build2
{
  using std::move;
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

  // <libbutl/utility.mxx>
  //
  using butl::reverse_iterate;
  using butl::compare_c_string;
  using butl::compare_pointer_target;
  //using butl::hash_pointer_target;
  using butl::combine_hash;
  using butl::casecmp;
  using butl::case_compare_string;
  using butl::case_compare_c_string;
  using butl::lcase;
  using butl::alpha;
  using butl::alnum;
  using butl::digit;

  using butl::trim;
  using butl::next_word;

  using butl::make_guard;
  using butl::make_exception_guard;

  using butl::getenv;
  using butl::setenv;
  using butl::unsetenv;

  using butl::throw_generic_error;
  using butl::throw_system_error;

  using butl::eof;

  extern bool stderr_term; // True if stderr is a terminal.

  // Command line options.
  //
  extern options ops;

  // Build system driver process path (argv0.initial is argv[0]).
  //
  extern process_path argv0;

  // Build system driver version and check.
  //
  extern const standard_version build_version;

  class location;

  void
  check_build_version (const standard_version_constraint&, const location&);

  // Work/home directories (must be initialized in main()) and relative path
  // calculation.
  //
  extern dir_path work;
  extern dir_path home;

  // By default this points to work. Setting this to something else should
  // only be done in tightly controlled, non-concurrent situations (e.g.,
  // state dump). If it is empty, then relative() below returns the original
  // path.
  //
  extern const dir_path* relative_base;

  // If possible and beneficial, translate an absolute, normalized path into
  // relative to the relative_base directory, which is normally work. Note
  // that if the passed path is the same as relative_base, then this function
  // returns empty path.
  //
  template <typename K>
  basic_path<char, K>
  relative (const basic_path<char, K>&);

  class path_target;

  path
  relative (const path_target&);

  // In addition to calling relative(), this function also uses shorter
  // notations such as '~/'. For directories the result includes the trailing
  // slash. If the path is the same as base, returns "./" if current is true
  // and empty string otherwise.
  //
  string
  diag_relative (const path&, bool current = true);

  // Diagnostics forward declarations (see diagnostics.hxx).
  //
  extern uint16_t verb;
  const  uint16_t verb_never = 7;

  // Basic process utilities.
  //
  // The run*() functions with process_path assume that you are printing
  // the process command line yourself.

  // Search for a process executable. Issue diagnostics and throw failed in
  // case of an error.
  //
  process_path
  run_search (const char*& args0,
              bool path_only,
              const location& = location ());

  inline process_path
  run_search (const char*& args0, const location& l = location ())
  {
    return run_search (args0, false, l);
  }

  process_path
  run_search (const path&,
              bool init = false,
              const dir_path& fallback = dir_path (),
              bool path_only = false,
              const location& = location ());

  // Wait for process termination. Issue diagnostics and throw failed in case
  // of abnormal termination. If the process has terminated normally but with
  // a non-zero exit status, then, if error is true, assume the diagnostics
  // has already been issued and throw failed as well. Otherwise (error is
  // false), return false. The last argument is used in cooperation with
  // run_start() in case STDERR is redirected to STDOUT.
  //
  bool
  run_finish (const char* args[],
              process&,
              bool error = true,
              const string& = string (),
              const location& = location ());

  inline void
  run_finish (cstrings& args, process& pr, const location& l = location ())
  {
    run_finish (args.data (), pr, true, string (), l);
  }

  // Start a process with the specified arguments. If in is -1, then redirect
  // STDIN to a pipe (can also be -2 to redirect to /dev/null or equivalent).
  // If out is -1, redirect STDOUT to a pipe. If error is false, then
  // redirecting STDERR to STDOUT (this can be used to suppress diagnostics
  // from the child process). Issue diagnostics and throw failed in case of an
  // error.
  //
  process
  run_start (uint16_t verbosity,
             const process_env&, // Implicit-constructible from process_path.
             const char* args[],
             int in,
             int out,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const location& = location ());

  inline process
  run_start (const process_env& pe, // Implicit-constructible from process_path.
             const char* args[],
             int in,
             int out,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const location& l = location ())
  {
    return run_start (verb_never, pe, args, in, out, error, cwd, l);
  }

  inline void
  run (const process_path& p,
       const char* args[],
       const dir_path& cwd = dir_path ())
  {
    process pr (run_start (p, args, 0 /* stdin */, 1 /* stdout */, true, cwd));
    run_finish (args, pr);
  }

  inline void
  run (const process_path& p,
       cstrings& args,
       const dir_path& cwd = dir_path ())
  {
    run (p, args.data (), cwd);
  }

  // As above, but search for the process (including updating args[0]) and
  // print the process commands line at the specified verbosity level.
  //
  inline process
  run_start (uint16_t verbosity,
             const char* args[],
             int in,
             int out,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const location& l = location ())
  {
    process_path pp (run_search (args[0], l));
    return run_start (verbosity, pp, args, in, out, error, cwd, l);
  }

  inline process
  run_start (uint16_t verbosity,
             cstrings& args,
             int in,
             int out,
             bool error = true,
             const dir_path& cwd = dir_path (),
             const location& l = location ())
  {
    return run_start (verbosity, args.data (), in, out, error, cwd, l);
  }

  inline void
  run (uint16_t verbosity,
       const char* args[],
       const dir_path& cwd = dir_path ())
  {
    process pr (run_start (verbosity,
                           args,
                           0 /* stdin */,
                           1 /* stdout */,
                           true,
                           cwd));
    run_finish (args, pr);
  }

  inline void
  run (uint16_t verbosity,
       cstrings& args,
       const dir_path& cwd = dir_path ())
  {
    run (verbosity, args.data (), cwd);
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

  // Empty/nullopt string, path, and project name.
  //
  extern const string       empty_string;
  extern const path         empty_path;
  extern const dir_path     empty_dir_path;
  extern const project_name empty_project_name;

  extern const optional<string>       nullopt_string;
  extern const optional<path>         nullopt_path;
  extern const optional<dir_path>     nullopt_dir_path;
  extern const optional<project_name> nullopt_project_name;

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
  struct variable;

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
  hash_options (sha256&, T&, const variable&);

  template <typename T>
  void
  hash_options (sha256&, T&, const char*);

  // As above but from the strings value directly.
  //
  class value;
  struct lookup;

  void
  append_options (cstrings&, const lookup&, const char* excl = nullptr);

  void
  append_options (strings&, const lookup&, const char* excl = nullptr);

  void
  hash_options (sha256&, const lookup&);

  void
  append_options (cstrings&, const strings&, const char* excl = nullptr);

  void
  append_options (strings&, const strings&, const char* excl = nullptr);

  void
  hash_options (sha256&, const strings&);

  void
  append_options (cstrings&,
                  const strings&, size_t,
                  const char* excl = nullptr);

  void
  append_options (strings&,
                  const strings&, size_t,
                  const char* excl = nullptr);

  void
  hash_options (sha256&, const strings&, size_t);

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
  hash_option_values (sha256&,
                      const char* opt,
                      I begin, I end,
                      F&& get = [] (const string& s) {return s;});

  // Check if a specified option is present in the variable or value. T is
  // either target or scope.
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

  bool
  find_option (const char* option, const lookup&, bool ignore_case = false);

  bool
  find_option (const char* option, const strings&, bool ignore_case = false);

  bool
  find_option (const char* option, const cstrings&, bool ignore_case = false);

  // As above but look for several options returning true if any is present.
  //
  template <typename T>
  bool
  find_options (initializer_list<const char*>,
                T&,
                const variable&,
                bool = false);

  template <typename T>
  bool
  find_options (initializer_list<const char*>, T&, const char*, bool = false);

  bool
  find_options (initializer_list<const char*>, const lookup&, bool = false);

  bool
  find_options (initializer_list<const char*>, const strings&, bool = false);

  bool
  find_options (initializer_list<const char*>, const cstrings&, bool = false);

  // As above but look for an option that has the specified prefix. Return the
  // pointer to option or NULL if not found (thus can be used as bool).
  // Search backward (which is normall consistent with how options override
  // each other).
  //
  template <typename T>
  const string*
  find_option_prefix (const char* prefix, T&, const variable&, bool = false);

  template <typename T>
  const string*
  find_option_prefix (const char* prefix, T&, const char*, bool = false);

  const string*
  find_option_prefix (const char* prefix, const lookup&, bool = false);

  const string*
  find_option_prefix (const char* prefix, const strings&, bool = false);

  const char*
  find_option_prefix (const char* prefix, const cstrings&, bool = false);

  // As above but look for several option prefixes.
  //
  template <typename T>
  const string*
  find_option_prefixes (initializer_list<const char*>,
                        T&,
                        const variable&,
                        bool = false);

  template <typename T>
  const string*
  find_option_prefixes (initializer_list<const char*>,
                        T&,
                        const char*,
                        bool = false);

  const string*
  find_option_prefixes (initializer_list<const char*>,
                        const lookup&, bool = false);

  const string*
  find_option_prefixes (initializer_list<const char*>,
                        const strings&,
                        bool = false);

  const char*
  find_option_prefixes (initializer_list<const char*>,
                        const cstrings&,
                        bool = false);

  // Apply the specified substitution (stem) to a '*'-pattern. If pattern is
  // NULL or empty, then return the stem itself. Assume the pattern is valid,
  // i.e., contains a single '*' character.
  //
  string
  apply_pattern (const char* stem, const string* pattern);

  // Initialize build2 global state (verbosity, home/work directories, etc).
  // Should be called early in main() once.
  //
  void
  init (const char* argv0, uint16_t verbosity);
}

#include <build2/utility.ixx>
#include <build2/utility.txx>

#endif // BUILD2_UTILITY_HXX
