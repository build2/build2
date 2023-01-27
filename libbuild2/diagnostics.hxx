// file      : libbuild2/diagnostics.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DIAGNOSTICS_HXX
#define LIBBUILD2_DIAGNOSTICS_HXX

#include <libbutl/diagnostics.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  struct diag_record;

  // Throw this exception to terminate the build. The handler should
  // assume that the diagnostics has already been issued.
  //
  class failed: public std::exception {};

  // Print low-verbosity recipe diagnostics in the forms:
  //
  // <prog> <l-target> <comb> <r-target>
  // <prog> <r-target>
  //
  // Where <prog> is an abbreviated/generalized program name, such as c++
  // (rather than g++ or clang++) or yacc (rather than bison or byacc),
  // <l-target> is typically the "main" prerequisite target, such as the C++
  // source file to compile, <r-target> is typically the target being
  // produced, and <comb> is the combiner, typically "->".
  //
  // The second form (without <l-target> and <comb>) should be used when there
  // is no natural "main" prerequisite, for example, for linking as well as
  // for programs that act upon the target, such as mkdir, rm, test, etc.
  //
  // Note also that these functions omit the @.../ qualification in either
  // <l-target> or <r-target> if it's implied by the other.
  //
  // For example:
  //
  // mkdir fsdir{details/}
  // c++ cxx{hello} -> obje{hello}
  // ld exe{hello}
  //
  // test exe{hello} + testscript
  //
  // install exe{hello} -> /usr/bin/
  // uninstall exe{hello} <- /usr/bin/
  //
  // rm exe{hello}
  // rm obje{hello}
  // rmdir fsdir{details/}
  //
  // Examples of target groups:
  //
  // cli cli{foo} -> {hxx cxx}{foo}
  //
  // thrift thrift{foo} -> {hxx cxx}{foo-types}
  //                       {hxx cxx}{foo-stubs}
  //
  // Potentially we could also support target groups for <l-target>:
  //
  // tool {hxx cxx}{foo} -> {hxx cxx}{foo-types}
  //
  // tool {hxx cxx}{foo-types}
  //      {hxx cxx}{foo-stubs} -> {hxx cxx}{foo-insts}
  //                              {hxx cxx}{foo-impls}
  //
  // Currently we only support this for the `group -> dir_path` form (used
  // by the backlink machinery).
  //
  // See also the `diag` Buildscript pseudo-builtin which is reduced to one of
  // the print_diag() calls (adhoc_buildscript_rule::print_custom_diag()). In
  // particular, if you are adding a new overload, also consider if/how it
  // should handled there.
  //
  // Note: see GH issue #40 for additional background and rationale.
  //
  // If <comb> is not specified, then "->" is used by default.

  // prog target -> target
  // prog target -> group
  //
  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const target& l, const target& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              target_key&& l, const target& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const target& l, target_key&& r,
              const char* comb = nullptr);

  void
  print_diag (const char* prog,
              target_key&& l, target_key&& r,
              const char* comb = nullptr);

  // Note: using small_vector would require target_key definition.
  //
  void
  print_diag (const char* prog,
              target_key&& l, vector<target_key>&& r,
              const char* comb = nullptr);

  // prog path -> target
  // prog path -> group
  //
  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const path& l, const target& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const path& l, target_key&& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const path& l, vector<target_key>&& r,
              const char* comb = nullptr);

  // prog string -> target
  // prog string -> group
  //
  // Use these versions if, for example, input information is passed as an
  // argument.
  //
  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const string& l, const target& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const string& l, target_key&& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const string& l, vector<target_key>&& r,
              const char* comb = nullptr);

  // prog target
  //
  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog, const target&);

  void
  print_diag (const char* prog, target_key&&);

  // prog group
  //
  void
  print_diag (const char* prog, vector<target_key>&&);

  // prog path
  //
  // Special versions for cases like mkdir/rmdir, save, etc.
  //
  // Note: use path_name("-") if the result is written to stdout.
  //
  void
  print_diag (const char* prog, const path&);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog, const dir_path&);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog, const path_name_view&);

  // Special versions for ln, cp, rm, install/unistall, dist, etc.
  //
  // Note: use path_name ("-") if the result is written to stdout.

  // prog target -> path
  //
  void
  print_diag (const char* prog,
              const target& l, const path& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const target& l, const dir_path& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const target& l, const path_name_view& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              target_key&& l, const dir_path& r,
              const char* comb = nullptr);

  // prog group -> dir_path
  //
  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              vector<target_key>&& l, const dir_path& r,
              const char* comb = nullptr);

  // prog path -> path
  //
  void
  print_diag (const char* prog,
              const path& l, const path& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const path& l, const dir_path& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const path& l, const path_name_view& r,
              const char* comb = nullptr);

  // prog string -> path
  //
  // Use this version if, for example, input information is passed as an
  // argument.
  //
  void
  print_diag (const char* prog,
              const string& l, const path& r,
              const char* comb = nullptr);

  LIBBUILD2_SYMEXPORT void
  print_diag (const char* prog,
              const string& l, const path_name_view& r,
              const char* comb = nullptr);

  // Print process commmand line. If the number of elements is specified (or
  // the const cstrings& version is used), then it will print the piped multi-
  // process command line, if present. In this case, the expected format is as
  // follows:
  //
  // name1 arg arg ... nullptr
  // name2 arg arg ... nullptr
  // ...
  // nameN arg arg ... nullptr nullptr
  //
  LIBBUILD2_SYMEXPORT void
  print_process (diag_record&,
                 const char* const* args, size_t n = 0);

  LIBBUILD2_SYMEXPORT void
  print_process (const char* const* args, size_t n = 0);

  inline void
  print_process (diag_record& dr,
                 const cstrings& args, size_t n = 0)
  {
    print_process (dr, args.data (), n != 0 ? n : args.size ());
  }

  inline void
  print_process (const cstrings& args, size_t n = 0)
  {
    print_process (args.data (), n != 0 ? n : args.size ());
  }

  // As above but with process_env.
  //
  LIBBUILD2_SYMEXPORT void
  print_process (diag_record&,
                 const process_env&, const char* const* args, size_t n = 0);

  LIBBUILD2_SYMEXPORT void
  print_process (const process_env&, const char* const* args, size_t n = 0);

  inline void
  print_process (diag_record& dr,
                 const process_env& pe, const cstrings& args, size_t n = 0)
  {
    print_process (dr, pe, args.data (), n != 0 ? n : args.size ());
  }

  inline void
  print_process (const process_env& pe, const cstrings& args, size_t n = 0)
  {
    print_process (pe, args.data (), n != 0 ? n : args.size ());
  }

  // Program verbosity level (-v/--verbose plus --silent).
  //
  // 0 - disabled
  // 1 - high-level information messages
  // 2 - essential underlying commands that are being executed
  // 3 - all underlying commands that are being executed
  // 4 - information helpful to the user (e.g., why a rule did not match)
  // 5 - information helpful to the developer
  // 6 - even more detailed information
  //
  // If silent is true, then the level must be 0 (silent is level 0 that
  // cannot be relaxed in certain contexts).
  //
  // While uint8 is more than enough, use uint16 for the ease of printing.
  //

  // Forward-declarated in <libbuild2/utility.hxx>.
  //
  // const uint16_t verb_never = 7;
  // extern uint16_t verb;
  // extern bool silent;

  template <typename F> inline void l1 (const F& f) {if (verb >= 1) f ();}
  template <typename F> inline void l2 (const F& f) {if (verb >= 2) f ();}
  template <typename F> inline void l3 (const F& f) {if (verb >= 3) f ();}
  template <typename F> inline void l4 (const F& f) {if (verb >= 4) f ();}
  template <typename F> inline void l5 (const F& f) {if (verb >= 5) f ();}
  template <typename F> inline void l6 (const F& f) {if (verb >= 6) f ();}

  // Stream verbosity level. Determined by the diagnostic type (e.g., trace
  // always has maximum verbosity) as well as the program verbosity. It is
  // used to decide whether to print relative/absolute paths and default
  // target extensions.
  //
  // Currently we have the following program to stream verbosity mapping:
  //
  // fail/error/warn/info   <2:{0,0}  2:{0,1} >2:{1,2}
  // trace                  *:{1,2}
  //
  // A stream that hasn't been (yet) assigned any verbosity explicitly (e.g.,
  // ostringstream) defaults to maximum.
  //
  struct stream_verbosity
  {
    union
    {
      struct
      {
        // 0 - print relative.
        // 1 - print absolute.
        //
        uint16_t path: 1;

        // 0 - don't print.
        // 1 - print if specified.
        // 2 - print as 'foo.?' if unspecified and 'foo.' if specified as
        //     "no extension" (empty).
        //
        uint16_t extension: 2;
      };
      uint16_t value_;
    };

    constexpr
    stream_verbosity (uint16_t p, uint16_t e): path (p), extension (e) {}

    explicit
    stream_verbosity (uint16_t v = 0): value_ (v) {}
  };

  constexpr stream_verbosity stream_verb_max = {1, 2};

  // Default program to stream verbosity mapping, as outlined above.
  //
  inline stream_verbosity
  stream_verb_map ()
  {
    return
      verb < 2 ? stream_verbosity (0, 0) :
      verb > 2 ? stream_verbosity (1, 2) :
      /*      */ stream_verbosity (0, 1);
  }

  LIBBUILD2_SYMEXPORT extern const int stream_verb_index;

  inline stream_verbosity
  stream_verb (ostream& os)
  {
    long v (os.iword (stream_verb_index));
    return v == 0
      ? stream_verb_max
      : stream_verbosity (static_cast<uint16_t> (v - 1));
  }

  inline void
  stream_verb (ostream& os, stream_verbosity v)
  {
    os.iword (stream_verb_index) = static_cast<long> (v.value_) + 1;
  }

  // Progress reporting.
  //
  using butl::diag_progress;
  using butl::diag_progress_lock;

  // Return true if progress is to be shown. The max_verb argument is the
  // maximum verbosity level that this type of progress should be shown at by
  // default. If it is verb_never, then both min and max verbosity checks are
  // omitted, assuming the caller takes care of that themselves.
  //
  inline bool
  show_progress (uint16_t max_verb)
  {
    return diag_progress_option
      ? *diag_progress_option
      : stderr_term && (max_verb == verb_never ||
                        (verb >= 1 && verb <= max_verb));
  }

  // Diagnostics color.
  //
  inline bool
  show_diag_color ()
  {
    return diag_color_option ? *diag_color_option : stderr_term_color;
  }

  // Diagnostic facility.
  //
  // Note that this is the "complex" case we we derive from (rather than
  // alias) a number of butl::diag_* types and provide custom operator<<
  // "overrides" in order to make ADL look in the build2 rather than butl
  // namespace.
  //
  using butl::diag_stream_lock;
  using butl::diag_stream;
  using butl::diag_epilogue;
  using butl::diag_frame;

  template <typename> struct diag_prologue;
  template <typename> struct diag_mark;

  struct diag_record: butl::diag_record
  {
    template <typename T>
    const diag_record&
    operator<< (const T& x) const
    {
      os << x;
      return *this;
    }

    diag_record () = default;

    template <typename B>
    explicit
    diag_record (const diag_prologue<B>& p): diag_record () { *this << p;}

    template <typename B>
    explicit
    diag_record (const diag_mark<B>& m): diag_record () { *this << m;}
  };

  template <typename B>
  struct diag_prologue: butl::diag_prologue<B>
  {
    using butl::diag_prologue<B>::diag_prologue;

    template <typename T>
    diag_record
    operator<< (const T& x) const
    {
      diag_record r;
      r.append (this->indent, this->epilogue);
      B::operator() (r);
      r << x;
      return r;
    }

    friend const diag_record&
    operator<< (const diag_record& r, const diag_prologue& p)
    {
      r.append (p.indent, p.epilogue);
      p (r);
      return r;
    }
  };

  template <typename B>
  struct diag_mark: butl::diag_mark<B>
  {
    using butl::diag_mark<B>::diag_mark;

    template <typename T>
    diag_record
    operator<< (const T& x) const
    {
      return B::operator() () << x;
    }

    friend const diag_record&
    operator<< (const diag_record& r, const diag_mark& m)
    {
      return r << m ();
    }
  };

  template <typename B>
  struct diag_noreturn_end: butl::diag_noreturn_end<B>
  {
    diag_noreturn_end () {} // For Clang 3.7 (const needs user default ctor).

    using butl::diag_noreturn_end<B>::diag_noreturn_end;

    [[noreturn]] friend void
    operator<< (const diag_record& r, const diag_noreturn_end& e)
    {
      assert (r.full ());
      e.B::operator() (r);
    }
  };

  // Note: diag frames are not applied to text/trace diagnostics.
  //
  template <typename F>
  struct diag_frame_impl: diag_frame
  {
    explicit
    diag_frame_impl (F f): diag_frame (&thunk), func_ (move (f)) {}

  private:
    static void
    thunk (const diag_frame& f, const butl::diag_record& r)
    {
      static_cast<const diag_frame_impl&> (f).func_ (
        static_cast<const diag_record&> (r));
    }

    const F func_;
  };

  template <typename F>
  inline diag_frame_impl<F>
  make_diag_frame (F f)
  {
    return diag_frame_impl<F> (move (f));
  }

  struct LIBBUILD2_SYMEXPORT simple_prologue_base
  {
    explicit
    simple_prologue_base (const char* type,
                          const char* mod,
                          const char* name,
                          stream_verbosity sverb)
        : type_ (type), mod_ (mod), name_ (name), sverb_ (sverb) {}

    void
    operator() (const diag_record& r) const;

  private:
    const char* type_;
    const char* mod_;
    const char* name_;
    const stream_verbosity sverb_;
  };

  struct LIBBUILD2_SYMEXPORT location_prologue_base
  {
    location_prologue_base (const char* type,
                            const char* mod,
                            const char* name,
                            const location& l,
                            stream_verbosity sverb)
        : type_ (type), mod_ (mod), name_ (name),
          loc_ (l),
          sverb_ (sverb) {}

    location_prologue_base (const char* type,
                            const char* mod,
                            const char* name,
                            const path_name_view& f,
                            stream_verbosity sverb)
        : type_ (type), mod_ (mod), name_ (name),
          loc_ (f),
          sverb_ (sverb) {}

    location_prologue_base (const char* type,
                            const char* mod,
                            const char* name,
                            path&& f,
                            stream_verbosity sverb)
        : type_ (type), mod_ (mod), name_ (name),
          file_ (move (f)), loc_ (file_),
          sverb_ (sverb) {}

    void
    operator() (const diag_record& r) const;

  private:
    const char* type_;
    const char* mod_;
    const char* name_;
    const path file_;
    const location loc_;
    const stream_verbosity sverb_;
  };

  struct basic_mark_base
  {
    using simple_prologue   = diag_prologue<simple_prologue_base>;
    using location_prologue = diag_prologue<location_prologue_base>;

    explicit
    basic_mark_base (const char* type,
                     const void* data = nullptr,
                     diag_epilogue* epilogue = &diag_frame::apply,
                     stream_verbosity (*sverb) () = &stream_verb_map,
                     const char* mod = nullptr,
                     const char* name = nullptr)
        : sverb_ (sverb),
          type_ (type), mod_ (mod), name_ (name), data_ (data),
          epilogue_ (epilogue) {}

    simple_prologue
    operator() () const
    {
      return simple_prologue (epilogue_, type_, mod_, name_, sverb_ ());
    }

    location_prologue
    operator() (const location& l) const
    {
      return location_prologue (epilogue_, type_, mod_, name_, l, sverb_ ());
    }

    location_prologue
    operator() (const location_value& l) const
    {
      return location_prologue (epilogue_, type_, mod_, name_, l, sverb_ ());
    }

    location_prologue
    operator() (const path_name& f) const
    {
      return location_prologue (epilogue_, type_, mod_, name_, f, sverb_ ());
    }

    location_prologue
    operator() (const path_name_view& f) const
    {
      return location_prologue (epilogue_, type_, mod_, name_, f, sverb_ ());
    }

    location_prologue
    operator() (const path_name_value& f) const
    {
      return location_prologue (epilogue_, type_, mod_, name_, f, sverb_ ());
    }

    // fail (relative (src)) << ...
    //
    location_prologue
    operator() (path&& f) const
    {
      return location_prologue (
        epilogue_, type_, mod_, name_, move (f), sverb_ ());
    }

    template <typename L>
    location_prologue
    operator() (const L& l) const
    {
      return location_prologue (
        epilogue_, type_, mod_, name_, get_location (l, data_), sverb_ ());
    }

  protected:
    stream_verbosity (*sverb_) ();
    const char* type_;
    const char* mod_;
    const char* name_;
    const void* data_;
    diag_epilogue* const epilogue_;
  };
  using basic_mark = diag_mark<basic_mark_base>;

  LIBBUILD2_SYMEXPORT extern const basic_mark error;
  LIBBUILD2_SYMEXPORT extern const basic_mark warn;
  LIBBUILD2_SYMEXPORT extern const basic_mark info;
  LIBBUILD2_SYMEXPORT extern const basic_mark text;

  // trace
  //
  struct trace_mark_base: basic_mark_base
  {
    explicit
    trace_mark_base (const char* name, const void* data = nullptr)
        : trace_mark_base (nullptr, name, data) {}

    trace_mark_base (const char* mod,
                     const char* name,
                     const void* data = nullptr)
        : basic_mark_base ("trace",
                           data,
                           nullptr, // No diag stack.
                           []() {return stream_verb_max;},
                           mod,
                           name) {}
  };
  using trace_mark = diag_mark<trace_mark_base>;
  using tracer = trace_mark;

  // fail
  //
  struct fail_mark_base: basic_mark_base
  {
    explicit
    fail_mark_base (const char* type,
                    const void* data = nullptr)
        : basic_mark_base (type,
                           data,
                           [](const butl::diag_record& r, butl::diag_writer* w)
                           {
                             diag_frame::apply (r);
                             r.flush (w);
                             throw failed ();
                           },
                           &stream_verb_map,
                           nullptr,
                           nullptr) {}
  };
  using fail_mark = diag_mark<fail_mark_base>;

  struct fail_end_base
  {
    [[noreturn]] void
    operator() (const diag_record& r) const
    {
      // If we just throw then the record's destructor will see an active
      // exception and will not flush the record.
      //
      r.flush ();
      throw failed ();
    }
  };
  using fail_end = diag_noreturn_end<fail_end_base>;

  LIBBUILD2_SYMEXPORT extern const fail_mark fail;
  LIBBUILD2_SYMEXPORT extern const fail_end  endf;

  // Diagnostics buffer.
  //
  // The purpose of this class is to handle diagnostics from child processes,
  // where handle can mean:
  //
  // - Buffer it (to avoid interleaving in parallel builds).
  //
  // - Stream it (if the input can be split into diagnostic records).
  //
  // - Do nothing (in serial builds or if requested not to buffer).
  //
  // In the future this class may also be responsible for converting the
  // diagnostics into the structured form (which means it may need to buffer
  // even in serial builds).
  //
  // The typical usage is as follows:
  //
  // process pr (..., diag_buffer::pipe (ctx));
  // diag_buffer dbuf (ctx, args[0], pr);  // Skip.
  // ifdstream is (move (pr.in_ofd));      // No skip.
  // ofdstream os (move (pr.out_fd));
  //
  // The reason for this somewhat roundabout setup is to make sure the
  // diag_buffer instance is destroyed before the process instance. This is
  // important in case an exception is thrown where we want to make sure all
  // our pipe ends are closed before we wait for the process exit (which
  // happens in the process destructor).
  //
  // And speaking of the destruction order, another thing to keep in mind is
  // that only one stream can use the skip mode (fdstream_mode::skip; because
  // skipping is performed in the blocking mode) and the stream that skips
  // should come first so that all other streams are destroyed/closed before
  // it (failed that, we may end up in a deadlock). For example:
  //
  // process pr (..., diag_buffer::pipe (ctx));
  // ifdstream is (move (pr.in_ofd), fdstream_mode::skip);      // Skip.
  // diag_buffer dbuf (ctx, args[0], pr, fdstream_mode::none);  // No skip.
  // ofdstream os (move (pr.out_fd));
  //
  class LIBBUILD2_SYMEXPORT diag_buffer
  {
  public:
    // If buffering is necessary or force is true, return an "instruction"
    // (-1) to the process class constructor to open a pipe and redirect
    // stderr to it. Otherwise, return an "instruction" to inherit stderr (2).
    //
    // The force flag is normally used if custom diagnostics processing is
    // required (filter, split, etc; see read() below).
    //
    // Note that the diagnostics buffer must be opened (see below) regardless
    // of the pipe() result.
    //
    static int
    pipe (context&, bool force = false);

    // Open the diagnostics buffer given the parent end of the pipe (normally
    // process:in_efd). If it is nullfd, then assume no buffering is
    // necessary. If mode is non_blocking, then make reading from the parent
    // end of the pipe non-blocking.
    //
    // The args0 argument is the child process program name for diagnostics.
    // It is expected to remain valid until the call to close() and should
    // normally be the same as args[0] passed to close().
    //
    // Note that the same buffer can go through multiple open-read-close
    // sequences, for example, to execute multiple commands.
    //
    // All the below functions handle io errors, issue suitable diagnostics,
    // and throw failed. If an exception is thrown from any of them, then the
    // instance should not be used any further.
    //
    // Note that when reading from multiple streams in the non-blocking mode,
    // only the last stream to be destroyed can normally have the skip mode
    // since in case of an exception, skipping will be blocking.
    //
    diag_buffer (context&,
                 const char* args0,
                 auto_fd&&,
                 fdstream_mode = fdstream_mode::skip);

    // As above, but the parrent end of the pipe (process:in_efd) is passed
    // via a process instance.
    //
    diag_buffer (context&,
                 const char* args0,
                 process&,
                 fdstream_mode = fdstream_mode::skip);

    // As above but with support for the underlying buffer reuse.
    //
    // Note that in most cases reusing the buffer is probably not worth the
    // trouble because we normally don't expect any diagnostics in the common
    // case. However, if needed, it can be arranged, for example:
    //
    // vector<char> buf;
    //
    // {
    //   process pr (...);
    //   diag_buffer dbuf (ctx, move (buf), args[0], pr);
    //   dbuf.read ();
    //   dbuf.close ();
    //   buf = move (dbuf.buf);
    // }
    //
    // {
    //   ...
    // }
    //
    // Note also that while there is no guarantee the underlying buffer is
    // moved when, say, the vector is empty, all the main implementations
    // always steal the buffer.
    //
    diag_buffer (context&,
                 vector<char>&& buf,
                 const char* args0,
                 auto_fd&&,
                 fdstream_mode = fdstream_mode::skip);

    diag_buffer (context&,
                 vector<char>&& buf,
                 const char* args0,
                 process&,
                 fdstream_mode = fdstream_mode::skip);

    // Separate construction and opening.
    //
    // Note: be careful with the destruction order (see above for details).
    //
    explicit
    diag_buffer (context&);

    diag_buffer (context&, vector<char>&& buf);

    void
    open (const char* args0,
          auto_fd&&,
          fdstream_mode = fdstream_mode::skip);

    // Open the buffer in the state as if after read() returned false, that
    // is, the stream corresponding to the parent's end of the pipe reached
    // EOF and has been closed. This is primarily useful when the diagnostics
    // is being read in a custom way (for example, it has been merged to
    // stdout) and all we want is to be able to call write() and close().
    //
    void
    open_eof (const char* args0);

    // Check whether the buffer has been opened with the open() call and
    // hasn't yet been closed.
    //
    // Note that this function returning true does not mean that the pipe was
    // opened (to check that, call is_open() on the stream member; see below).
    //
    bool
    is_open () const
    {
      return state_ != state::closed;
    }

    // Read the diagnostics from the parent's end of the pipe if one was
    // opened and buffer/stream it as necessary or forced. Return true if
    // there could be more diagnostics to read (only possible in the non-
    // blocking mode) and false otherwise, in which case also close the
    // stream.
    //
    // Note that the force argument here (as well as in write() below) and
    // in open() above are independent. Specifically, force in open() forces
    // the opening of the pipe while force in read() and write() forces
    // the buffering of the diagnostics.
    //
    // Instead of calling this function you can perform custom reading and, if
    // necessary, buffering of the diagnostics by accessing the input stream
    // (is) and underlying buffer (buf) directly. This can be used to filter,
    // split the diagnostics into records according to a certain format, etc.
    // Note that such custom processing implementation should maintain the
    // overall semantics of diagnostics buffering in that it may only omit
    // buffering in the serial case or if the diagnostics can be streamed in
    // atomic records. See also write() below.
    //
    // The input stream is opened in the text mode and has the badbit but not
    // failbit exception mask. The custom processing should also be compatible
    // with the stream mode (blocking or non). If buffering is performed, then
    // depending on the expected diagnostics the custom processing may want to
    // reserve an appropriate initial buffer size to avoid unnecessary
    // reallocation. As a convenience, in the blocking mode only, if the
    // stream still contains some diagnostics, then it can be handled by
    // calling read(). This is useful when needing to process only the inital
    // part of the diagnostics. The custom processing may also close the
    // stream manually before calling close().
    //
    bool
    read (bool force = false);

    // Close the parent end of the pipe if one was opened and write out any
    // buffered diagnostics.
    //
    // If the child process exited abnormally or normally with non-0 code,
    // then print the error diagnostics to this effect. Additionally, if the
    // verbosity level is between 1 and the specified value, then print the
    // command line as info after the error. If omit_normal is true, then
    // don't print either for the normal exit (usually used for custom
    // diagnostics or when process failure can be tolerated).
    //
    // Normally the specified verbosity will be 1 and the command line args
    // represent the verbosity level 2 (logical) command line. Note that args
    // should only represent a single command in a pipe (see print_process()
    // below for details).
    //
    // If the diag_buffer instance is destroyed before calling close(), then
    // any buffered diagnostics is discarded.
    //
    // Note: see also run_finish(diag_buffer&).
    //
    // @@ TODO: need overload with process_env (see print_process). Also in
    //          run_finish_impl().
    //
    void
    close (const cstrings& args,
           const process_exit&,
           uint16_t verbosity,
           bool omit_normal = false,
           const location& = {});

    void
    close (const char* const* args,
           const process_exit&,
           uint16_t verbosity,
           bool omit_normal = false,
           const location& = {});

    // As above but with a custom diag record for the child exit diagnostics,
    // if any. Note that if the diag record has the fail epilogue, then this
    // function will throw.
    //
    void
    close (diag_record&& = {});

    // Direct access to the underlying stream and buffer for custom processing
    // (see read() above for details).
    //
    // If serial is true, then we are running serially. If nobuf is true,
    // then we are running in parallel but diagnostics buffering has been
    // disabled (--no-diag-buffer). Note that there is a difference: during
    // the serial execution we are free to hold the diag_stream_lock for as
    // long as convenient, for example, for the whole duration of child
    // process execution. Doing the same during parallel execution is very
    // bad idea and we should read/write the diagnostics in chunks, normally
    // one line at a time.
    //
  public:
    ifdstream    is;
    vector<char> buf;
    const char*  args0;
    bool         serial;
    bool         nobuf;

    // Buffer or stream a fragment of diagnostics as necessary or forced. If
    // newline is true, also add a trailing newline.
    //
    // This function is normally called from a custom diagnostics processing
    // implementation (see read() above for details). If nobuf is true, then
    // the fragment should end on the line boundary to avoid interleaving.
    //
    void
    write (const string&, bool newline, bool force = false);

  private:
    // Note that we don't seem to need a custom destructor to achieve the
    // desired semantics: we can assume the process has exited before we are
    // destroyed (because we supply stderr to its constructor) which means
    // closing fdstream without reading any futher should be ok.
    //
    enum class state {closed, opened, eof};

    context& ctx_;
    state    state_ = state::closed;
  };

  // Action phrases, e.g., "configure update exe{foo}", "updating exe{foo}",
  // and "updating exe{foo} is configured". Use like this:
  //
  // info << "while " << diag_doing (a, t);
  //
  struct diag_phrase
  {
    const action& a;
    const target& t;
    void (*f) (ostream&, const action&, const target&);
  };

  inline ostream&
  operator<< (ostream& os, const diag_phrase& p)
  {
    p.f (os, p.a, p.t);
    return os;
  }

  LIBBUILD2_SYMEXPORT string
  diag_do (context&, const action&);

  LIBBUILD2_SYMEXPORT void
  diag_do (ostream&, const action&, const target&);

  inline diag_phrase
  diag_do (const action& a, const target& t)
  {
    return diag_phrase {a, t, &diag_do};
  }

  LIBBUILD2_SYMEXPORT string
  diag_doing (context&, const action&);

  LIBBUILD2_SYMEXPORT void
  diag_doing (ostream&, const action&, const target&);

  inline diag_phrase
  diag_doing (const action& a, const target& t)
  {
    return diag_phrase {a, t, &diag_doing};
  }

  LIBBUILD2_SYMEXPORT string
  diag_did (context&, const action&);

  LIBBUILD2_SYMEXPORT void
  diag_did (ostream&, const action&, const target&);

  inline diag_phrase
  diag_did (const action& a, const target& t)
  {
    return diag_phrase {a, t, &diag_did};
  }

  LIBBUILD2_SYMEXPORT void
  diag_done (ostream&, const action&, const target&);

  inline diag_phrase
  diag_done (const action& a, const target& t)
  {
    return diag_phrase {a, t, &diag_done};
  }
}

#include <libbuild2/diagnostics.ixx>

#endif // LIBBUILD2_DIAGNOSTICS_HXX
