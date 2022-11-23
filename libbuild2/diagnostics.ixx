// file      : libbuild2/diagnostics.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  // print_diag()
  //
  LIBBUILD2_SYMEXPORT void
  print_diag_impl (const char*, target_key*, target_key&&, const char*);

  LIBBUILD2_SYMEXPORT void
  print_diag_impl (const char*,
                   target_key*, vector<target_key>&& r,
                   const char*);

  inline void
  print_diag (const char* p, target_key&& l, target_key&& r, const char* c)
  {
    print_diag_impl (p, &l, move (r), c);
  }

  inline void
  print_diag (const char* p,
              target_key&& l, vector<target_key>&& r,
              const char* c)
  {
    print_diag_impl (p, &l, move (r), c);
  }

  inline void
  print_diag (const char* p, target_key& r)
  {
    print_diag_impl (p, nullptr, move (r), nullptr);
  }

  inline void
  print_diag (const char* p, vector<target_key>&& r)
  {
    print_diag_impl (p, nullptr, move (r), nullptr);
  }

  inline void
  print_diag (const char* p, const path& r)
  {
    print_diag (p, path_name (&r));
  }

  inline void
  print_diag (const char* p, const target& l, const path& r, const char* c)
  {
    print_diag (p, l, path_name (&r), c);
  }

  inline void
  print_diag (const char* p, const path& l, const path& r, const char* c)
  {
    print_diag (p, l, path_name (&r), c);
  }

  inline void
  print_diag (const char* p, const string& l, const path& r, const char* c)
  {
    print_diag (p, l, path_name (&r), c);
  }

  // diag_buffer
  //
  inline diag_buffer::
  diag_buffer (context& ctx)
      : is (ifdstream::badbit), ctx_ (ctx)
  {
  }

  inline diag_buffer::
  diag_buffer (context& ctx, vector<char>&& b)
      : is (ifdstream::badbit), buf (move (b)), ctx_ (ctx)
  {
    buf.clear ();
  }

  inline diag_buffer::
  diag_buffer (context& ctx, const char* args0, auto_fd&& fd, fdstream_mode m)
      : diag_buffer (ctx)
  {
    open (args0, move (fd), m);
  }

  inline diag_buffer::
  diag_buffer (context& ctx, const char* args0, process& pr, fdstream_mode m)
      : diag_buffer (ctx)
  {
    open (args0, move (pr.in_efd), m);
  }

  inline diag_buffer::
  diag_buffer (context& ctx,
               vector<char>&& b,
               const char* args0,
               auto_fd&& fd,
               fdstream_mode m)
      : diag_buffer (ctx, move (b))
  {
    open (args0, move (fd), m);
  }

  inline diag_buffer::
  diag_buffer (context& ctx,
               vector<char>&& b,
               const char* args0,
               process& pr,
               fdstream_mode m)
      : diag_buffer (ctx, move (b))
  {
    open (args0, move (pr.in_efd), m);
  }

  inline void diag_buffer::
  close (const cstrings& args,
         const process_exit& pe,
         uint16_t verbosity,
         bool omit_normal,
         const location& loc)
  {
    close (args.data (), pe, verbosity, omit_normal, loc);
  }
}
