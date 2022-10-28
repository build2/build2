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
  // This class is also responsible for converting the diagnostics into the
  // structured form (which means it may need to buffer even in serial
  // builds).
  //
  class LIBBUILD2_SYMEXPORT diag_buffer
  {
  public:
    explicit
    diag_buffer (context& c): is (ifdstream::badbit), ctx_ (c) {}

  public:
    // If buffering is necessary or force is true, open a pipe and return the
    // child process end of it. Otherwise, return stderr. If blocking is
    // false, then make reading from the parent end of the pipe non-blocking.
    //
    // The args0 argument is the child process program name for diagnostics.
    // It is expected to remain valid until the call to close() and should
    // normally be the same as args[0] passed to close().
    //
    // The force flag is used if custom diagnostics processing is required
    // (filter, split, etc; see read() below).
    //
    // Note that the same buffer can go through multiple open-read-close
    // sequences, for example, to execute multiple commands.
    //
    // All the below functions handle io errors, issue suitable diagnostics,
    // and throw failed. If an exception is thrown from any of them, then the
    // instance should not be used any further.
    //
    process::pipe
    open (const char* args0, bool force = false, bool blocking = true);

    // Read the diagnostics from the parent end of the pipe if one was opened
    // and buffer/stream it as necessary. Return true if there could be more
    // diagnostics to read (only possible in the non-blocking mode).
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
    // reallocation. As a convenience, in the blocking mode, if the stream
    // still contains some diagnostics, then it can be handled by calling
    // read(). This is helpful when needing to process only the inital part of
    // the diagnostics.
    //
    bool
    read ();

    // Close the parent end of the pipe if one was opened and write out any
    // buffered diagnostics.
    //
    // If the verbosity level is between 1 and the specified value and the
    // child process exited with non-0 code, then print the command line after
    // the diagnostics. Normally the specified verbosity will be 1 and the
    // command line args represent the verbosity level 2 (logical) command
    // line. The semantics os the args/args_size arguments is the same as
    // in print_process() below.
    //
    // If the diag_buffer instance is destroyed before calling close(), then
    // any buffered diagnostics is discarded.
    //
    // @@ TODO: need overload with process_env (see print_process).
    //
    void
    close (const cstrings& args,
           const process_exit& pe,
           uint16_t verbosity = 1,
           const location& loc = {})
    {
      close (args.data (), args.size (), pe, verbosity, loc);
    }

    void
    close (const char* const* args,
           const process_exit& pe,
           uint16_t verbosity = 1,
           const location& loc = {})
    {
      close (args, 0, pe, verbosity, loc);
    }

    void
    close (const char* const* args, size_t args_size,
           const process_exit& pe,
           uint16_t verbosity = 1,
           const location& loc = {});


    // This version calls close() plus it first waits for the process to
    // finish and later throws failed if it didn't exit with 0 code (so
    // similar to run_finish ()).
    //
    void
    finish (const cstrings& args,
            process& pr,
            uint16_t verbosity = 1,
            const location& loc = {})
    {
      finish (args.data (), args.size (), pr, verbosity, loc);
    }

    void
    finish (const char* const* args,
            process& pr,
            uint16_t verbosity = 1,
            const location& loc = {})
    {
      finish (args, 0, pr, verbosity, loc);
    }

    void
    finish (const char* const* args, size_t args_size,
            process&,
            uint16_t verbosity = 1,
            const location& = {});

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

    // Buffer or stream a fragment of diagnostics as necessary. If newline
    // is true, also add a trailing newline.
    //
    // This function is normally called from a custom diagnostics processing
    // implementation (see read() above for details). If nobuf is true, then
    // the fragment should end on the line boundary to avoid interleaving.
    //
    void
    write (const string&, bool newline);

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
  // maximum verbosity level that this type of progress should be shown by
  // default.
  //
  inline bool
  show_progress (uint16_t max_verb)
  {
    return diag_progress_option
      ? *diag_progress_option
      : stderr_term && verb >= 1 && verb <= max_verb;
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
                           [](const butl::diag_record& r)
                           {
                             diag_frame::apply (r);
                             r.flush ();
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

#endif // LIBBUILD2_DIAGNOSTICS_HXX
