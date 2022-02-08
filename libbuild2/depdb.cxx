// file      : libbuild2/depdb.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/depdb.hxx>

#ifdef _WIN32
#  include <libbutl/win32-utility.hxx>
#endif

#include <libbuild2/filesystem.hxx>  // mtime()
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // Note that state::write with absent pos is interpreted as non-existent.
  //
  depdb_base::
  depdb_base (const path& p, bool ro, state s, optional<uint64_t> pos)
      : state_ (s), ro_ (ro)
  {
    if (s == state::write && ro)
    {
      new (&is_) ifdstream ();
      buf_ = nullptr; // Shouldn't be needed.
      return;
    }

    fdopen_mode om (fdopen_mode::binary);
    ifdstream::iostate em (ifdstream::badbit);

    if (s == state::write)
    {
      om |= fdopen_mode::out;

      if (!pos)
        om |= fdopen_mode::create | fdopen_mode::exclusive;

      em |= ifdstream::failbit;
    }
    else
    {
      om |= fdopen_mode::in;

      // Both in & out so can switch from read to write.
      //
      if (!ro)
        om |= fdopen_mode::out;
    }

    auto_fd fd;
    try
    {
      fd = fdopen (p, om);
    }
    catch (const io_error&)
    {
      bool c (s == state::write && !pos);

      diag_record dr (fail);
      dr << "unable to " << (c ? "create " : "open ") << p;

      if (c)
        dr << info << "did you forget to add fsdir{} prerequisite for "
           << "output directory?";

      dr << endf;
    }

    if (pos)
    try
    {
      fdseek (fd.get (), *pos, fdseek_mode::set);
    }
    catch (const io_error& e)
    {
      fail << "unable to rewind " << p << ": " << e;
    }

    // Open the corresponding stream. Note that if we throw after that, the
    // corresponding member will not be destroyed. This is the reason for the
    // depdb/base split.
    //
    if (state_ == state::read)
    {
      new (&is_) ifdstream (move (fd), em);
      buf_ = static_cast<fdstreambuf*> (is_.rdbuf ());
    }
    else
    {
      new (&os_) ofdstream (move (fd), em, pos ? *pos : 0);
      buf_ = static_cast<fdstreambuf*> (os_.rdbuf ());
    }
  }

  depdb::
  depdb (path_type&& p, bool ro, timestamp mt)
      : depdb_base (p,
                    ro,
                    mt != timestamp_nonexistent ? state::read : state::write),
        path (move (p)),
        mtime (mt != timestamp_nonexistent ? mt : timestamp_unknown)
  {
    // Read/write the database format version.
    //
    if (state_ == state::read)
    {
      string* l (read ());
      if (l != nullptr && *l == "1")
        return;
    }

    if (!ro)
      write ('1');
    else if (reading ())
      change ();
  }

  depdb::
  depdb (path_type p, bool ro)
      : depdb (move (p), ro, build2::mtime (p))
  {
  }

  depdb::
  depdb (reopen_state rs)
      : depdb_base (rs.path, false, state::write, rs.pos),
        path (move (rs.path)),
        mtime (timestamp_unknown),
        touch (rs.mtime)
  {
  }

  void depdb::
  change (bool trunc)
  {
    assert (state_ != state::write);

    if (ro_)
    {
      buf_ = nullptr;
    }
    else
    {
      // Transfer the file descriptor from ifdstream to ofdstream. Note that
      // the steps in this dance must be carefully ordered to make sure we
      // don't call any destructors twice in the face of exceptions.
      //
      auto_fd fd (is_.release ());

      // Consider this scenario: we are overwriting an old line (so it ends
      // with a newline and the "end marker") but the operation failed half
      // way through. Now we have the prefix from the new line, the suffix
      // from the old, and everything looks valid. So what we need is to
      // somehow invalidate the old content so that it can never combine with
      // (partial) new content to form a valid line. One way to do that would
      // be to truncate the file.
      //
      if (trunc)
      try
      {
        fdtruncate (fd.get (), pos_);
      }
      catch (const io_error& e)
      {
        fail << "unable to truncate " << path << ": " << e;
      }

      // Note: the file descriptor position can be beyond the pos_ value due
      // to the ifdstream buffering. That's why we need to seek to switch from
      // reading to writing.
      //
      try
      {
        fdseek (fd.get (), pos_, fdseek_mode::set);
      }
      catch (const io_error& e)
      {
        fail << "unable to rewind " << path << ": " << e;
      }

      // @@ Strictly speaking, ofdstream can throw which will leave us in a
      //    non-destructible state. Unlikely but possible.
      //
      is_.~ifdstream ();
      new (&os_) ofdstream (move (fd),
                            ofdstream::badbit | ofdstream::failbit,
                            pos_);
      buf_ = static_cast<fdstreambuf*> (os_.rdbuf ());
    }

    state_ = state::write;
    mtime = timestamp_unknown;
  }

  string* depdb::
  read_ ()
  {
    // Save the start position of this line so that we can overwrite it.
    //
    pos_ = buf_->tellg ();

    try
    {
      // Note that we intentionally check for eof after updating the write
      // position.
      //
      if (state_ == state::read_eof)
        return nullptr;

      getline (is_, line_); // Calls line_.erase().

      // The line should always end with a newline. If it doesn't, then this
      // line (and the rest of the database) is assumed corrupted. Also peek
      // at the character after the newline. We should either have the next
      // line or '\0', which is our "end marker", that is, it indicates the
      // database was properly closed.
      //
      ifdstream::int_type c;
      if (is_.fail () || // Nothing got extracted.
          is_.eof ()  || // Eof reached before delimiter.
          (c = is_.peek ()) == ifdstream::traits_type::eof ())
      {
        // Preemptively switch to writing. While we could have delayed this
        // until the user called write(), if the user calls read() again (for
        // whatever misguided reason) we will mess up the overwrite position.
        //
        change ();
        return nullptr;
      }

      // Handle the "end marker". Note that the caller can still switch to the
      // write mode on this line. And, after calling read() again, write to
      // the next line (i.e., start from the "end marker").
      //
      if (c == '\0')
        state_ = state::read_eof;
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << path << ": " << e;
    }

    return &line_;
  }

  bool depdb::
  skip ()
  {
    if (state_ == state::read_eof)
      return true;

    assert (state_ == state::read);

    // The rest is pretty similar in logic to read_() above.
    //
    pos_ = buf_->tellg ();

    try
    {
      // Keep reading lines checking for the end marker after each newline.
      //
      ifdstream::int_type c;
      do
      {
        if ((c = is_.get ()) == '\n')
        {
          if ((c = is_.get ()) == '\0')
          {
            state_ = state::read_eof;
            return true;
          }
        }
      } while (c != ifdstream::traits_type::eof ());
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << path << ": " << e;
    }

    // Invalid database so change over to writing.
    //
    change ();
    return false;
  }

  void depdb::
  write (const char* s, size_t n, bool nl)
  {
    // Switch to writing if we are still reading.
    //
    if (state_ != state::write)
      change ();

    try
    {
      os_.write (s, static_cast<streamsize> (n));

      if (nl)
        os_.put ('\n');
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << path << ": " << e;
    }
  }

  void depdb::
  write (char c, bool nl)
  {
    // Switch to writing if we are still reading.
    //
    if (state_ != state::write)
      change ();

    try
    {
      os_.put (c);

      if (nl)
        os_.put ('\n');
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << path << ": " << e;
    }
  }

  void depdb::
  close (bool mc)
  {
    if (ro_)
    {
      is_.close ();
      return;
    }

    // If we are at eof, then it means all lines are good, there is the "end
    // marker" at the end, and we don't need to do anything, except, maybe
    // touch the file. Otherwise, if we are still in the read mode, truncate
    // the rest, and then add the "end marker" (we cannot have anything in the
    // write mode since we truncate in change()).
    //
    // Note that we handle touch with timestamp_unknown specially by making a
    // modification to the file (which happens naturally in the write mode)
    // and letting the filesystem update its mtime.
    //
    if (state_ == state::read_eof)
    {
      if (!touch)
      try
      {
        is_.close ();
        return;
      }
      catch (const io_error& e)
      {
        fail << "unable to close " << path << ": " << e;
      }

      // While there are utime(2)/utimensat(2) (and probably something similar
      // for Windows), for now we just overwrite the "end marker". Hopefully
      // no implementation will be smart enough to recognize this is a no-op
      // and skip updating mtime (which would probably be incorrect, spec-
      // wise). And this could even be faster since we already have the file
      // descriptor. Or it might be slower since so far we've only been
      // reading.
      //
      // Note also that utime() on Windows is a bad idea (see touch_file() for
      // details).
      //
      if (*touch == timestamp_unknown)
      {
        pos_ = buf_->tellg ();         // The last line is accepted.
        change (false /* truncate */); // Write end marker below.
      }
    }
    else if (state_ != state::write)
    {
      pos_ = buf_->tellg (); // The last line is accepted.
      change (true /* truncate */);
    }

    if (mc && mtime_check ())
      start_ = system_clock::now ();

    if (state_ == state::write)
    try
    {
      os_.put ('\0'); // The "end marker".
      os_.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to flush file " << path << ": " << e;
    }

    if (touch && *touch != timestamp_unknown)
    try
    {
      file_mtime (path, *touch);
    }
    catch (const system_error& e)
    {
      fail << "unable to touch file " << path << ": " << e;
    }

    // On some platforms (currently confirmed on FreeBSD running as VMs) one
    // can sometimes end up with a modification time that is a bit after the
    // call to close(). And in some tight cases this can mess with our
    // "protocol" that a valid depdb should be no older than the target it is
    // for.
    //
    // Note that this does not seem to be related to clock adjustments but
    // rather feels like the modification time is set when the changes
    // actually hit some lower-level layer (e.g., OS or filesystem
    // driver). One workaround that appears to work is to query the
    // mtime. This seems to force that layer to commit to a timestamp.
    //
#if defined(__FreeBSD__)
    mtime = build2::mtime (path); // Save for debugging/check below.
#endif
  }

  depdb::reopen_state depdb::
  close_to_reopen ()
  {
    assert (!touch);

    if (state_ != state::write)
    {
      pos_ = buf_->tellg (); // The last line is accepted.
      change (state_ != state::read_eof /* truncate */);
    }

    pos_ = buf_->tellp ();

    try
    {
      os_.put ('\0'); // The "end marker".
      os_.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to flush file " << path << ": " << e;
    }

    // Note: must still be done for FreeBSD if changing anything here (see
    // close() for details).
    //
    mtime = build2::mtime (path);

    return reopen_state {move (path), pos_, mtime};
  }

  void depdb::
  check_mtime_ (const path_type& t, timestamp e)
  {
    // We could call the static version but then we would have lost additional
    // information for some platforms.
    //
    timestamp t_mt (build2::mtime (t));

    if (t_mt == timestamp_nonexistent)
      fail << "target file " << t << " does not exist at the end of recipe";

    timestamp d_mt (build2::mtime (path));

    if (d_mt > t_mt)
    {
      if (e == timestamp_unknown)
        e = system_clock::now ();

      fail << "backwards modification times detected:\n"
           << "    " << start_ << " sequence start\n"
#if defined(__FreeBSD__)
           << "    " << mtime  << " close mtime\n"
#endif
           << "    " << d_mt   << " " << path.string () << '\n'
           << "    " << t_mt   << " " << t.string () << '\n'
           << "    " << e      << " sequence end";
    }
  }

  void depdb::
  check_mtime_ (timestamp s,
                const path_type& d,
                const path_type& t,
                timestamp e)
  {
    using build2::mtime;

    timestamp t_mt (mtime (t));

    if (t_mt == timestamp_nonexistent)
      fail << "target file " << t << " does not exist at the end of recipe";

    timestamp d_mt (mtime (d));

    if (d_mt > t_mt)
    {
      fail << "backwards modification times detected:\n"
           << "    " << s    << " sequence start\n"
           << "    " << d_mt << " " << d.string () << '\n'
           << "    " << t_mt << " " << t.string () << '\n'
           << "    " << e    << " sequence end";
    }
  }
}
