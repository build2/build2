// file      : libbuild2/diagnostics.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/diagnostics.hxx>

#include <cstring>  // strchr(), memcpy()

#include <libbutl/process-io.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/action.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // Diagnostics state (verbosity level, progress, etc). Keep disabled until
  // set from options.
  //
  uint16_t verb = 0;
  bool silent = true;

  optional<bool> diag_progress_option;

  bool diag_no_line = false;
  bool diag_no_column = false;

  bool stderr_term = false;

  void
  init_diag (uint16_t v, bool s, optional<bool> p, bool nl, bool nc, bool st)
  {
    assert (!s || v == 0);

    verb = v;
    silent = s;
    diag_progress_option = p;
    diag_no_line = nl;
    diag_no_column = nc;
    stderr_term = st;
  }

  // Stream verbosity.
  //
  const int stream_verb_index = ostream::xalloc ();

  // diag_buffer
  //
  process::pipe diag_buffer::
  open (const char* args0, bool force, fdstream_mode m)
  {
    assert (state_ == state::closed && args0 != nullptr);

    serial = ctx_.sched.serial ();
    nobuf = !serial && ctx_.no_diag_buffer;

    process::pipe r;
    if (!(serial || nobuf) || force)
    {
      try
      {
        fdpipe p (fdopen_pipe ());

        // Note that we must return non-owning fd to our end of the pipe (see
        // the process class for details).
        //
        r = process::pipe (p.in.get (), move (p.out));

        m |= fdstream_mode::text;

        is.open (move (p.in), m);
      }
      catch (const io_error& e)
      {
        fail << "unable to read from " << args0 << " stderr: " << e;
      }
    }
    else
      r = process::pipe (-1, 2);

    this->args0 = args0;
    state_ = state::opened;
    return r;
  }

  bool diag_buffer::
  read ()
  {
    assert (state_ == state::opened);

    bool r;
    if (is.is_open ())
    {
      try
      {
        // Copy buffers directly.
        //
        auto copy = [this] (fdstreambuf& sb)
        {
          const char* p (sb.gptr ());
          size_t n (sb.egptr () - p);

          // Allocate at least fdstreambuf::buffer_size to reduce
          // reallocations and memory fragmentation.
          //
          size_t i (buf.size ());
          if (i == 0 && n < fdstreambuf::buffer_size)
            buf.reserve (fdstreambuf::buffer_size);

          buf.resize (i + n);
          memcpy (buf.data () + i, p, n);

          sb.gbump (static_cast<int> (n));
        };

        if (is.blocking ())
        {
          if (serial || nobuf)
          {
            // This is the case where we are called after custom processing.
            //
            assert (buf.empty ());

            // Note that the eof check is important: if the stream is at eof,
            // this and all subsequent writes to the diagnostics stream will
            // fail (and you won't see a thing).
            //
            if (is.peek () != ifdstream::traits_type::eof ())
            {
              if (serial)
              {
                // Holding the diag lock while waiting for diagnostics from
                // the child process would be a bad idea in the parallel
                // build. But it should be harmless in serial.
                //
                // @@ TODO: do direct buffer copy.
                //
                diag_stream_lock dl;
                *diag_stream << is.rdbuf ();
              }
              else
              {
                // Read/write one line at a time not to hold the lock for too
                // long.
                //
                for (string l; !eof (std::getline (is, l)); )
                {
                  diag_stream_lock dl;
                  *diag_stream << l << '\n';
                }
              }
            }
          }
          else
          {
            fdstreambuf& sb (*static_cast<fdstreambuf*> (is.rdbuf ()));

            while (is.peek () != istream::traits_type::eof ())
              copy (sb);
          }

          r = false;
        }
        else
        {
          // We do not support finishing off after the custom processing in
          // the non-blocking mode (but could probably do if necessary).
          //
          assert (!(serial || nobuf));

          fdstreambuf& sb (*static_cast<fdstreambuf*> (is.rdbuf ()));

          // Try not to allocate the buffer if there is no diagnostics (the
          // common case).
          //
          // Note that we must read until blocked (0) or EOF (-1).
          //
          streamsize n;
          while ((n = sb.in_avail ()) > 0)
            copy (sb);

          r = (n != -1);
        }

        if (!r)
          is.close ();
      }
      catch (const io_error& e)
      {
        // For now we assume (here and pretty much everywhere else) that the
        // output can't fail.
        //
        fail << "unable to read from " << args0 << " stderr: " << e;
      }
    }
    else
      r = false;

    if (!r)
      state_ = state::eof;

    return r;
  }

  void diag_buffer::
  write (const string& s, bool nl)
  {
    // Similar logic to read() above.
    //
    if (serial || nobuf)
    {
      assert (buf.empty ());

      diag_stream_lock dl;
      *diag_stream << s;
      if (nl)
        *diag_stream << '\n';
    }
    else
    {
      size_t n (s.size () + (nl ? 1 : 0));

      size_t i (buf.size ());
      if (i == 0 && n < fdstreambuf::buffer_size)
        buf.reserve (fdstreambuf::buffer_size);

      buf.resize (i + n);
      memcpy (buf.data () + i, s.c_str (), s.size ());

      if (nl)
        buf.back () = '\n';
    }
  }

  void diag_buffer::
  close (const char* const* args,
         const process_exit& pe,
         uint16_t v,
         const location& loc,
         bool omit_normall)
  {
    tracer trace ("diag_buffer::close");

    assert (state_ != state::closed);

    // We need to make sure the command line we print on the unsuccessful exit
    // is inseparable from any buffered diagnostics. So we prepare the record
    // first and then write both while holding the diagnostics stream lock.
    //
    diag_record dr;
    if (!pe)
    {
      // Note: see similar code in run_finish_impl().
      //
      if (omit_normall && pe.normal ())
      {
        l4 ([&]{trace << "process " << args[0] << " " << pe;});
      }
      else
      {
        // It's unclear whether we should print this only if printing the
        // command line (we could also do things differently for
        // normal/abnormal exit). Let's print this always and see how it
        // wears.
        //
        // Note: make sure keep the above trace is not printing.
        //
        dr << error (loc) << "process " << args[0] << " " << pe;

        if (verb >= 1 && verb <= v)
        {
          dr << info << "command line: ";
          print_process (dr, args);
        }
      }
    }

    close (move (dr));
  }

  void diag_buffer::
  close (diag_record&& dr)
  {
    assert (state_ != state::closed);

    // We may still be in the open state in case of custom processing.
    //
    if (state_ == state::opened)
    {
      if (is.is_open ())
      {
        try
        {
          if (is.good ())
          {
            if (is.blocking ())
            {
              assert (is.peek () == ifdstream::traits_type::eof ());
            }
            else
            {
              assert (is.rdbuf ()->in_avail () == -1);
            }
          }

          is.close ();
        }
        catch (const io_error& e)
        {
          fail << "unable to read from " << args0 << " stderr: " << e;
        }
      }

      state_ = state::eof;
    }

    if (!buf.empty () || !dr.empty ())
    {
      diag_stream_lock l;

      if (!buf.empty ())
        diag_stream->write (buf.data (), static_cast<streamsize> (buf.size ()));

      if (!dr.empty ())
        dr.flush ([] (const butl::diag_record& r)
                  {
                    // Similar to default_writer().
                    //
                    *diag_stream << r.os.str () << '\n';
                  });

      diag_stream->flush ();
    }

    buf.clear ();
    args0 = nullptr;
    state_ = state::closed;
  }

  // print_process()
  //
  void
  print_process (const char* const* args, size_t n)
  {
    diag_record dr (text);
    print_process (dr, args, n);
  }

  void
  print_process (diag_record& dr,
                 const char* const* args, size_t n)
  {
    dr << butl::process_args {args, n};
  }

  void
  print_process (const process_env& pe, const char* const* args, size_t n)
  {
    diag_record dr (text);
    print_process (dr, pe, args, n);
  }

  void
  print_process (diag_record& dr,
                 const process_env& pe, const char* const* args, size_t n)
  {
    if (pe.env ())
      dr << pe << ' ';

    dr << butl::process_args {args, n};
  }

  // Diagnostic facility, project specifics.
  //

  void simple_prologue_base::
  operator() (const diag_record& r) const
  {
    stream_verb (r.os, sverb_);

    if (type_ != nullptr)
      r << type_ << ": ";

    if (mod_ != nullptr)
      r << mod_ << "::";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  void location_prologue_base::
  operator() (const diag_record& r) const
  {
    stream_verb (r.os, sverb_);

    if (!loc_.empty ())
    {
      r << loc_.file << ':';

      if (!diag_no_line)
      {
        if (loc_.line != 0)
        {
          r << loc_.line << ':';

          if (!diag_no_column)
          {
            if (loc_.column != 0)
              r << loc_.column << ':';
          }
        }
      }

      r << ' ';
    }

    if (type_ != nullptr)
      r << type_ << ": ";

    if (mod_ != nullptr)
      r << mod_ << "::";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  const basic_mark error ("error");
  const basic_mark warn  ("warning");
  const basic_mark info  ("info");
  const basic_mark text  (nullptr, nullptr, nullptr); // No type/data/frame.
  const fail_mark  fail  ("error");
  const fail_end   endf;

  // diag_do(), etc.
  //
  string
  diag_do (context& ctx, const action&)
  {
    const meta_operation_info& m (*ctx.current_mif);
    const operation_info& io (*ctx.current_inner_oif);
    const operation_info* oo (ctx.current_outer_oif);

    string r;

    // perform(update(x))   -> "update x"
    // configure(update(x)) -> "configure updating x"
    //
    if (m.name_do.empty ())
      r = io.name_do;
    else
    {
      r = m.name_do;

      if (io.name_doing[0] != '\0')
      {
        r += ' ';
        r += io.name_doing;
      }
    }

    if (oo != nullptr)
    {
      r += " (for ";
      r += oo->name;
      r += ')';
    }

    return r;
  }

  void
  diag_do (ostream& os, const action& a, const target& t)
  {
    os << diag_do (t.ctx, a) << ' ' << t;
  }

  string
  diag_doing (context& ctx, const action&)
  {
    const meta_operation_info& m (*ctx.current_mif);
    const operation_info& io (*ctx.current_inner_oif);
    const operation_info* oo (ctx.current_outer_oif);

    string r;

    // perform(update(x))   -> "updating x"
    // configure(update(x)) -> "configuring updating x"
    //
    if (!m.name_doing.empty ())
      r = m.name_doing;

    if (io.name_doing[0] != '\0')
    {
      if (!r.empty ()) r += ' ';
      r += io.name_doing;
    }

    if (oo != nullptr)
    {
      r += " (for ";
      r += oo->name;
      r += ')';
    }

    return r;
  }

  void
  diag_doing (ostream& os, const action& a, const target& t)
  {
    os << diag_doing (t.ctx, a) << ' ' << t;
  }

  string
  diag_did (context& ctx, const action&)
  {
    const meta_operation_info& m (*ctx.current_mif);
    const operation_info& io (*ctx.current_inner_oif);
    const operation_info* oo (ctx.current_outer_oif);

    string r;

    // perform(update(x))   -> "updated x"
    // configure(update(x)) -> "configured updating x"
    //
    if (!m.name_did.empty ())
    {
      r = m.name_did;

      if (io.name_doing[0] != '\0')
      {
        r += ' ';
        r += io.name_doing;
      }
    }
    else
      r += io.name_did;

    if (oo != nullptr)
    {
      r += " (for ";
      r += oo->name;
      r += ')';
    }

    return r;
  }

  void
  diag_did (ostream& os, const action& a, const target& t)
  {
    os << diag_did (t.ctx, a) << ' ' << t;
  }

  void
  diag_done (ostream& os, const action&, const target& t)
  {
    const meta_operation_info& m (*t.ctx.current_mif);
    const operation_info& io (*t.ctx.current_inner_oif);
    const operation_info* oo (t.ctx.current_outer_oif);

    // perform(update(x))   -> "x is up to date"
    // configure(update(x)) -> "updating x is configured"
    //
    if (m.name_done.empty ())
    {
      os << t;

      if (io.name_done[0] != '\0')
        os << ' ' << io.name_done;

      if (oo != nullptr)
        os << " (for " << oo->name << ')';
    }
    else
    {
      if (io.name_doing[0] != '\0')
        os << io.name_doing << ' ';

      if (oo != nullptr)
        os << "(for " << oo->name << ") ";

      os << t << ' ' << m.name_done;
    }
  }
}
