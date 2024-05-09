// file      : libbuild2/diagnostics.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/diagnostics.hxx>

#include <cstring> // strchr(), memcpy()
#include <cstdlib> // getenv()

#include <libbutl/fdstream.hxx>   // fdterm_color()
#include <libbutl/process-io.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/action.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // Diagnostics state (verbosity level, progress, etc). Keep default/disabled
  // until set from options.
  //
  uint16_t verb = 1;
  bool silent = false;

  optional<bool> diag_progress_option;
  optional<bool> diag_color_option;

  bool diag_no_line = false;
  bool diag_no_column = false;

  bool stderr_term = false;
  bool stderr_term_color = false;

  void
  init_diag (uint16_t v,
             bool s,
             optional<bool> p,
             optional<bool> c,
             bool nl,
             bool nc,
             bool st)
  {
    assert (!s || v == 0);

    verb = v;
    silent = s;
    diag_progress_option = p;
    diag_color_option = c;
    diag_no_line = nl;
    diag_no_column = nc;
    stderr_term = st;

    if (st)
    {
      // Only attempt to enable if explicitly requested by the user. Note that
      // while we may enable color for our process, who knows if this gets
      // inherited by other processes we start (e.g., compilers) and/or
      // whether they will do something sensible about any of this.
      //
      try
      {
        stderr_term_color = fdterm_color (stderr_fd (), c && *c /* enable */);
      }
      catch (const io_error& e)
      {
        fail << "unable to query terminal color support for stderr: " << e;
      }

      // If the user specified --diag-color on POSIX we will trust the color
      // is supported (e.g., wrong TERM value, etc).
      //
      if (!stderr_term_color && c && *c)
      {
#ifdef _WIN32
        fail << "unable to enable diagnostics color support for stderr";
#else
        stderr_term_color = true;
#endif
      }
    }
    else
      stderr_term_color = false;
  }

  // Stream verbosity.
  //
  const int stream_verb_index = ostream::xalloc ();

  // print_diag()
  //
  void
  print_diag_impl (const char* p, target_key* l, target_key&& r, const char* c)
  {
    // @@ Print directly to diag_stream (and below)? Won't we be holding
    //    the lock longer?

    diag_record dr (text);

    dr << p << ' ';

    if (l != nullptr)
    {
      // Omit the @.../ qualification in either lhs or rhs if it's implied by
      // the other.
      //
      // @@ Shouldn't we, strictly speaking, also check that they belong to
      //    the same project? Though it would be far-fetched to use another
      //    project's target from src. Or maybe not.
      //
      if (!l->out->empty ())
      {
        if (r.out->empty ())
          l->out = &empty_dir_path;
      }
      else if (!r.out->empty ())
        r.out = &empty_dir_path;

      dr << *l << ' ' << (c == nullptr ? "->" : c) << ' ';
    }

    dr << r;
  }


  static inline bool
  print_diag_cmp (const pair<optional<string>, const target_key*>& x,
                  const pair<optional<string>, const target_key*>& y)
  {
    return (x.second->dir->compare (*y.second->dir) == 0 &&
            x.first->compare (*y.first) == 0);
  }

  // Return true if we have multiple partitions (see below for details).
  //
  static bool
  print_diag_collect (const vector<target_key>& tks,
                      ostringstream& os,
                      stream_verbosity sv,
                      vector<pair<optional<string>, const target_key*>>& ns)
  {
    ns.reserve (tks.size ());

    for (const target_key& k: tks)
    {
      bool r;
      if (auto p = k.type->print)
        r = p (os, k, true /* name_only */);
      else
        r = to_stream (os, k, sv, true /* name_only */);

      ns.push_back (make_pair (r ? optional<string> (os.str ()) : nullopt, &k));

      os.clear ();
      os.str (string ()); // Note: just seekp(0) is not enough.
    }

    // Partition.
    //
    // While at it also determine whether we have multiple partitions.
    //
    bool ml (false);
    for (auto b (ns.begin ()), e (ns.end ()); b != e; )
    {
      const pair<optional<string>, const target_key*>& x (*b++);

      // Move all the elements that are equal to x to the front, preserving
      // order.
      //
      b = stable_partition (
        b, e,
        [&x] (const pair<optional<string>, const target_key*>& y)
        {
          return (x.first && y.first && print_diag_cmp (x, y));
        });

      if (!ml && b != e)
        ml = true;
    }

    return ml;
  }

  static void
  print_diag_print (const vector<pair<optional<string>, const target_key*>>& ns,
                    ostringstream& os,
                    stream_verbosity sv,
                    const optional<string>& ml)
  {
    for (auto b (ns.begin ()), i (b), e (ns.end ()); i != e; )
    {
      if (i != b)
        os << '\n' << *ml;

      const pair<optional<string>, const target_key*>& p (*i);

      if (!p.first) // Irregular.
      {
        os << *p.second;
        ++i;
        continue;
      }

      // Calculate the number of members in this partition.
      //
      size_t n (1);
      for (auto j (i + 1); j != e && j->first && print_diag_cmp (*i, *j); ++j)
        ++n;

      // Similar code to to_stream(target_key).
      //

      // Print the directory.
      //
      {
        const target_key& k (*p.second);

        uint16_t dv (sv.path);

        // Note: relative() returns empty for './'.
        //
        const dir_path& rd (dv < 1 ? relative (*k.dir) : *k.dir);

        if (!rd.empty ())
        {
          if (dv < 1)
            os << diag_relative (rd);
          else
            to_stream (os, rd, true /* representation */);
        }
      }

      // Print target types.
      //
      {
        if (n != 1)
          os << '{';

        for (auto j (i), e (i + n); j != e; ++j)
          os << (j != i ? " " : "") << j->second->type->name;

        if (n != 1)
          os << '}';
      }

      // Print the target name (the same for all members of this partition).
      //
      os << '{' << *i->first << '}';

      i += n;
    }
  }

  template <typename L> // L can be target_key, path, or string.
  static void
  print_diag_impl (const char* p,
                   const L* l, bool lempty,
                   vector<target_key>&& rs,
                   const char* c)
  {
    assert (rs.size () > 1);

    // The overall plan is as follows:
    //
    // 1. Collect the printed names for all the group members.
    //
    //    Note if the printed representation is irregular (see
    //    to_stream(target_key) for details). We will print such members each
    //    on a separate line.
    //
    // 2. Move the names around so that we end up with contiguous partitions
    //    of targets with the same name.
    //
    // 3. Print the partitions, one per line.
    //
    // The steps 1-2 are performed by print_diag_impl_common() above.
    //
    vector<pair<optional<string>, const target_key*>> ns;

    // Use the diag_record's ostringstream so that we get the appropriate
    // stream verbosity, etc.
    //
    diag_record dr (text);
    ostringstream& os (dr.os);
    stream_verbosity sv (stream_verb (os));

    optional<string> ml;
    if (print_diag_collect (rs, os, sv, ns))
      ml = string ();

    // Print.
    //
    os << p << ' ';

    if (l != nullptr)
      os << *l << (lempty ? "" : " ") << (c == nullptr ? "->" : c) << ' ';

    if (ml)
      ml = string (os.str ().size (), ' '); // Indentation.

    print_diag_print (ns, os, sv, ml);
  }

  template <typename R> // R can be target_key, path, or string.
  static void
  print_diag_impl (const char* p,
                   vector<target_key>&& ls, const R& r,
                   const char* c)
  {
    assert (ls.size () > 1);

    // As above but for the group on the LHS.
    //
    vector<pair<optional<string>, const target_key*>> ns;

    diag_record dr (text);
    ostringstream& os (dr.os);
    stream_verbosity sv (stream_verb (os));

    optional<string> ml;
    if (print_diag_collect (ls, os, sv, ns))
      ml = string ();

    // Print.
    //
    os << p << ' ';

    if (ml)
      ml = string (os.str ().size (), ' '); // Indentation.

    print_diag_print (ns, os, sv, ml);

    // @@ TODO: make sure `->` is aligned with longest line printed by
    //    print_diag_print(). Currently it can look like this:
    //
    // ln /tmp/hello-gcc/hello/hello/{hxx cxx}{hello-types}
    //    /tmp/hello-gcc/hello/hello/{hxx cxx}{hello-stubs}
    //    /tmp/hello-gcc/hello/hello/cxx{hello-ext} -> ./
    //
    os << ' ' << (c == nullptr ? "->" : c) << ' ' << r;
  }

  void
  print_diag_impl (const char* p,
                   target_key* l, vector<target_key>&& rs,
                   const char* c)
  {
    // Note: keep this implementation separate from the above for performance.
    //
    assert (!rs.empty ());

    if (rs.size () == 1)
    {
      print_diag_impl (p, l, move (rs.front ()), c);
      return;
    }

    // At the outset handle out-qualification as above. Here we assume that
    // all the targets in the group have the same out.
    //
    if (l != nullptr)
    {
      if (!l->out->empty ())
      {
        if (rs.front ().out->empty ())
          l->out = &empty_dir_path;
      }
      else if (!rs.front ().out->empty ())
      {
        for (target_key& r: rs)
          r.out = &empty_dir_path;
      }
    }

    print_diag_impl<target_key> (p, l, false /* empty */, move (rs), c);
  }

  // Note: these can't be inline since need the target class definition.
  //
  void
  print_diag (const char* p, const target& l, const target& r, const char* c)
  {
    target_key lk (l.key ());
    print_diag_impl (p, &lk, r.key (), c);
  }

  void
  print_diag (const char* p, target_key&& l, const target& r, const char* c)
  {
    print_diag_impl (p, &l, r.key (), c);
  }

  void
  print_diag (const char* p, const target& l, target_key&& r, const char* c)
  {
    target_key lk (l.key ());
    print_diag_impl (p, &lk, move (r), c);
  }

  void
  print_diag (const char* p, const path& l, const target& r, const char* c)
  {
    return print_diag (p, l, r.key (), c);
  }

  void
  print_diag (const char* p, const path& l, target_key&& r, const char* c)
  {
    text << p << ' ' << l << ' ' << (c == nullptr ? "->" : c) << ' ' << r;
  }

  void
  print_diag (const char* p,
              const path& l, vector<target_key>&& rs,
              const char* c)
  {
    assert (!rs.empty ());

    if (rs.size () == 1)
      print_diag (p, l, move (rs.front ()), c);
    else
      print_diag_impl<path> (p, &l, false /* empty */, move (rs), c);
  }

  void
  print_diag (const char* p, const string& l, const target& r, const char* c)
  {
    return print_diag (p, l, r.key (), c);
  }

  void
  print_diag (const char* p, const string& l, target_key&& r, const char* c)
  {
    text << p << ' '
         << l << (l.empty () ? "" : " ")
         << (c == nullptr ? "->" : c) << ' '
         << r;
  }

  void
  print_diag (const char* p,
              const string& l, vector<target_key>&& rs,
              const char* c)
  {
    assert (!rs.empty ());

    if (rs.size () == 1)
      print_diag (p, l, move (rs.front ()), c);
    else
      print_diag_impl<string> (p, &l, l.empty (), move (rs), c);
  }

  void
  print_diag (const char* p, const target& r)
  {
    print_diag_impl (p, nullptr, r.key (), nullptr);
  }

  void
  print_diag (const char* p, const dir_path& r)
  {
    text << p << ' ' << r;
  }

  void
  print_diag (const char* p, const path_name_view& r)
  {
    text << p << ' ' << r;
  }

  void
  print_diag (const char* p,
              const target& l, const path_name_view& r,
              const char* c)
  {
    // @@ TODO: out qualification stripping: only do if p.out is subdir of t
    //          (also below)?

    text << p << ' ' << l << ' ' << (c == nullptr ? "->" : c) << ' ' << r;
  }

  void
  print_diag (const char* p, const target& l, const dir_path& r, const char* c)
  {
    print_diag (p, l.key (), r, c);
  }

  void
  print_diag (const char* p, target_key&& l, const dir_path& r, const char* c)
  {
    text << p << ' ' << l << ' ' << (c == nullptr ? "->" : c) << ' ' << r;
  }

  void
  print_diag (const char* p,
              vector<target_key>&& ls, const dir_path& r,
              const char* c)
  {
    assert (!ls.empty ());

    if (ls.size () == 1)
      print_diag (p, move (ls.front ()), r, c);
    else
      print_diag_impl<dir_path> (p, move (ls), r, c);
  }

  void
  print_diag (const char* p, const path& l, const dir_path& r, const char* c)
  {
    text << p << ' ' << l << ' ' << (c == nullptr ? "->" : c) << ' ' << r;
  }

  void
  print_diag (const char* p,
              const path& l, const path_name_view& r,
              const char* c)
  {
    text << p << ' ' << l << ' ' << (c == nullptr ? "->" : c) << ' ' << r;
  }

  void
  print_diag (const char* p,
              const string& l, const path_name_view& r,
              const char* c)
  {
    text << p << ' '
         << l << (l.empty () ? "" : " ")
         << (c == nullptr ? "->" : c) << ' '
         << r;
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

  // diag_buffer
  //

  int diag_buffer::
  pipe (context& ctx, bool force)
  {
    return (ctx.sched->serial () || ctx.no_diag_buffer) && !force ? 2 : -1;
  }

  void diag_buffer::
  open (const char* args0, auto_fd&& fd, fdstream_mode m)
  {
    assert (state_ == state::closed && args0 != nullptr);

    serial = ctx_.sched->serial ();
    nobuf = !serial && ctx_.no_diag_buffer;

    if (fd != nullfd)
    {
      try
      {
        is.open (move (fd), m | fdstream_mode::text);
      }
      catch (const io_error& e)
      {
        fail << "unable to read from " << args0 << " stderr: " << e;
      }
    }

    this->args0 = args0;
    state_ = state::opened;
  }

  void diag_buffer::
  open_eof (const char* args0)
  {
    assert (state_ == state::closed && args0 != nullptr);

    serial = ctx_.sched->serial ();
    nobuf = !serial && ctx_.no_diag_buffer;
    this->args0 = args0;
    state_ = state::eof;
  }

  bool diag_buffer::
  read (bool force)
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
          if ((serial || nobuf) && !force)
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
          // the non-blocking mode unless forced to buffer (but could probably
          // do if necessary).
          //
          assert (!(serial || nobuf) || force);

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
  write (const string& s, bool nl, bool force)
  {
    assert (state_ != state::closed);

    // Similar logic to read() above.
    //
    if ((serial || nobuf) && !force)
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
         bool omit_normal,
         const location& loc)
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
      if (omit_normal && pe.normal ())
      {
        l4 ([&]{trace << "process " << args[0] << " " << pe;});
      }
      else
      {
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

    // Note: flushing of the diag record may throw.
    //
    args0 = nullptr;
    state_ = state::closed;

    if (!buf.empty () || !dr.empty ())
    {
      diag_stream_lock l;

      if (!buf.empty ())
      {
        diag_stream->write (buf.data (), static_cast<streamsize> (buf.size ()));
        buf.clear ();
      }

      if (!dr.empty ())
        dr.flush ([] (const butl::diag_record& r)
                  {
                    // Similar to default_writer().
                    //
                    *diag_stream << r.os.str () << '\n';
                    diag_stream->flush ();
                  });
      else
        diag_stream->flush ();
    }
  }

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
