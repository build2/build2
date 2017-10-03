// file      : build2/diagnostics.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_DIAGNOSTICS_HXX
#define BUILD2_DIAGNOSTICS_HXX

#include <libbutl/diagnostics.mxx>

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  using butl::diag_record;

  // Throw this exception to terminate the build. The handler should
  // assume that the diagnostics has already been issued.
  //
  class failed: public std::exception {};

  // Print process commmand line. If the number of elements is specified
  // (or the second version is used), then it will print the piped multi-
  // process command line, if present. In this case, the expected format
  // is as follows:
  //
  // name1 arg arg ... nullptr
  // name2 arg arg ... nullptr
  // ...
  // nameN arg arg ... nullptr nullptr
  //
  void
  print_process (diag_record&, const char* const* args, size_t n = 0);

  void
  print_process (const char* const* args, size_t n = 0);

  inline void
  print_process (diag_record& dr, const cstrings& args)
  {
    print_process (dr, args.data (), args.size ());
  }

  inline void
  print_process (const cstrings& args)
  {
    print_process (args.data (), args.size ());
  }

  // Program verbosity level (-v/--verbose).
  //
  // 0 - disabled
  // 1 - high-level information messages
  // 2 - essential underlying commands that are being executed
  // 3 - all underlying commands that are being executed
  // 4 - information helpful to the user (e.g., why a rule did not match)
  // 5 - information helpful to the developer
  // 6 - even more detailed information
  //
  // While uint8 is more than enough, use uint16 for the ease of printing.
  //
  extern uint16_t verb;
  const  uint16_t verb_never = 7;

  template <typename F> inline void l1 (const F& f) {if (verb >= 1) f ();}
  template <typename F> inline void l2 (const F& f) {if (verb >= 2) f ();}
  template <typename F> inline void l3 (const F& f) {if (verb >= 3) f ();}
  template <typename F> inline void l4 (const F& f) {if (verb >= 4) f ();}
  template <typename F> inline void l5 (const F& f) {if (verb >= 5) f ();}
  template <typename F> inline void l6 (const F& f) {if (verb >= 6) f ();}

  // Stream verbosity level. It is determined by the diagnostic type (e.g.,
  // trace always has maximum verbosity) as well as the program verbosity. It
  // is used to decide whether to print relative/absolute paths, and default
  // target extensions.
  //
  // 0 - minimum
  // 1 - intermediate
  // 2 - maximum
  //
  // Currently we have the following program to stream verbosity mapping:
  //
  // fail/error/warn/info   <2:0  2:1 >2:2
  // trace                  *:2
  //
  // A stream that hasn't been (yet) assigned any verbosity explicitly (e.g.,
  // ostringstream) defaults to maximum.
  //
  const uint16_t stream_verb_min = 0;
  const uint16_t stream_verb_max = 2;

  // Default program to stream verbosity mapping, as outlined above.
  //
  inline uint16_t
  stream_verb_map () {return verb < 2 ? 0 : (verb > 2 ? 2 : 1);}

  extern const int stream_verb_index;

  inline uint16_t
  stream_verb (ostream& os)
  {
    uint16_t v (static_cast<uint16_t> (os.iword (stream_verb_index)));
    return v == 0 ? stream_verb_max : v - 1;
  }

  inline void
  stream_verb (ostream& os, uint16_t v)
  {
    os.iword (stream_verb_index) = static_cast<long> (v + 1);
  }

  // Diagnostic facility, base infrastructure.
  //
  using butl::diag_stream_lock;
  using butl::diag_stream;
  using butl::diag_epilogue;

  // Diagnostics stack. Each frame is "applied" to the fail/error/warn/info
  // diag record.
  //
  // Unfortunately most of our use-cases don't fit into the 2-pointer small
  // object optimization of std::function. So we have to complicate things
  // a bit here.
  //
  struct diag_frame
  {
    explicit
    diag_frame (void (*f) (const diag_frame&, const diag_record&))
        : func_ (f), prev_ (stack) {stack = this;}

    // Start with an existing stack, for example, from another thread.
    //
    explicit
    diag_frame (const diag_frame* prev)
        : prev_ (stack) {stack = prev;} // Just a restore guard.

    static void
    apply (const diag_record& r)
    {
      for (const diag_frame* f (stack); f != nullptr; f = f->prev_)
        f->func_ (*f, r);
    }

    ~diag_frame () {stack = prev_;}

    static
#ifdef __cpp_thread_local
    thread_local
#else
    __thread
#endif
    const diag_frame* stack; // Tip of the stack.

  private:
    void (*func_) (const diag_frame&, const diag_record&);
    const diag_frame* prev_;
  };

  template <typename F>
  struct diag_frame_impl: diag_frame
  {
    explicit
    diag_frame_impl (F f): diag_frame (&thunk), func_ (move (f)) {}

  private:
    static void
    thunk (const diag_frame& f, const diag_record& r)
    {
      static_cast<const diag_frame_impl&> (f).func_ (r);
    }

    const F func_;
  };

  template <typename F>
  inline diag_frame_impl<F>
  make_diag_frame (F f)
  {
    return diag_frame_impl<F> (move (f));
  }

  // Diagnostic facility, project specifics.
  //
  struct simple_prologue_base
  {
    explicit
    simple_prologue_base (const char* type,
                          const char* mod,
                          const char* name,
                          uint16_t sverb)
        : type_ (type), mod_ (mod), name_ (name), sverb_ (sverb) {}

    void
    operator() (const diag_record& r) const;

  private:
    const char* type_;
    const char* mod_;
    const char* name_;
    const uint16_t sverb_;
  };

  class location
  {
  public:
    // Note that location maintains a shallow reference to path. Zero lines
    // or columns are not printed.
    //
    location (): file (nullptr), line (0), column (0) {}
    location (const path* f, uint64_t l = 0, uint64_t c = 0)
        : file (f), line (l), column (c) {}

    bool
    empty () const {return file == nullptr;}

    const path* file;
    uint64_t line;
    uint64_t column;
  };

  struct location_prologue_base
  {
    location_prologue_base (const char* type,
                            const char* mod,
                            const char* name,
                            const location& l,
                            uint16_t sverb)
        : type_ (type), mod_ (mod), name_ (name),
          loc_ (l),
          sverb_ (sverb) {}

    location_prologue_base (const char* type,
                            const char* mod,
                            const char* name,
                            path&& f,
                            uint16_t sverb)
        : type_ (type), mod_ (mod), name_ (name),
          file_ (move (f)), loc_ (&file_),
          sverb_ (sverb) {}

    void
    operator() (const diag_record& r) const;

  private:
    const char* type_;
    const char* mod_;
    const char* name_;
    const path file_;
    const location loc_;
    const uint16_t sverb_;
  };

  struct basic_mark_base
  {
    using simple_prologue   = butl::diag_prologue<simple_prologue_base>;
    using location_prologue = butl::diag_prologue<location_prologue_base>;

    explicit
    basic_mark_base (const char* type,
                     const void* data = nullptr,
                     diag_epilogue* epilogue = &diag_frame::apply,
                     uint16_t (*sverb) () = &stream_verb_map,
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
    uint16_t (*sverb_) ();
    const char* type_;
    const char* mod_;
    const char* name_;
    const void* data_;
    diag_epilogue* const epilogue_;
  };
  using basic_mark = butl::diag_mark<basic_mark_base>;

  extern const basic_mark error;
  extern const basic_mark warn;
  extern const basic_mark info;
  extern const basic_mark text;

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
  using trace_mark = butl::diag_mark<trace_mark_base>;
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
                           [](const diag_record& r)
                           {
                             diag_frame::apply (r);
                             r.flush ();
                             throw failed ();
                           },
                           &stream_verb_map,
                           nullptr,
                           nullptr) {}
  };
  using fail_mark = butl::diag_mark<fail_mark_base>;

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
  using fail_end = butl::diag_noreturn_end<fail_end_base>;

  extern const fail_mark fail;
  extern const fail_end  endf;
}

#endif // BUILD2_DIAGNOSTICS_HXX
