// file      : libbuild2/diagnostics.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DIAGNOSTICS_HXX
#define LIBBUILD2_DIAGNOSTICS_HXX

#include <libbutl/diagnostics.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

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
  LIBBUILD2_SYMEXPORT void
  print_process (diag_record&, const char* const* args, size_t n = 0);

  LIBBUILD2_SYMEXPORT void
  print_process (const char* const* args, size_t n = 0);

  inline void
  print_process (diag_record& dr, const cstrings& args, size_t n = 0)
  {
    print_process (dr, args.data (), n != 0 ? n : args.size ());
  }

  inline void
  print_process (const cstrings& args, size_t n = 0)
  {
    print_process (args.data (), n != 0 ? n : args.size ());
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
  struct LIBBUILD2_SYMEXPORT diag_frame
  {
    explicit
    diag_frame (void (*f) (const diag_frame&, const diag_record&))
        : func_ (f)
    {
      if (func_ != nullptr)
        prev_ = stack (this);
    }

    diag_frame (diag_frame&& x)
        : func_ (x.func_)
    {
      if (func_ != nullptr)
      {
        prev_ = x.prev_;
        stack (this);

        x.func_ = nullptr;
      }
    }

    diag_frame& operator= (diag_frame&&) = delete;

    diag_frame (const diag_frame&) = delete;
    diag_frame& operator= (const diag_frame&) = delete;

    ~diag_frame ()
    {
      if (func_ != nullptr )
        stack (prev_);
    }

    static void
    apply (const diag_record& r)
    {
      for (const diag_frame* f (stack ()); f != nullptr; f = f->prev_)
        f->func_ (*f, r);
    }

    // Tip of the stack.
    //
    static const diag_frame*
    stack () noexcept;

    // Set the new and return the previous tip of the stack.
    //
    static const diag_frame*
    stack (const diag_frame*) noexcept;

    struct stack_guard
    {
      explicit stack_guard (const diag_frame* s): s_ (stack (s)) {}
      ~stack_guard () {stack (s_);}
      const diag_frame* s_;
    };

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
    using simple_prologue   = butl::diag_prologue<simple_prologue_base>;
    using location_prologue = butl::diag_prologue<location_prologue_base>;

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
  using basic_mark = butl::diag_mark<basic_mark_base>;

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
