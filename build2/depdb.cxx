// file      : build2/depdb.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/depdb.hxx>

#include <libbutl/filesystem.mxx> // file_mtime()

#ifdef _WIN32
#  include <libbutl/win32-utility.hxx>
#endif

#include <build2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  depdb_base::
  depdb_base (const path& p, timestamp mt)
  {
    fdopen_mode om (fdopen_mode::out | fdopen_mode::binary);
    ifdstream::iostate em (ifdstream::badbit);

    if (mt == timestamp_nonexistent)
    {
      state_ = state::write;
      om |= fdopen_mode::create | fdopen_mode::exclusive;
      em |= ifdstream::failbit;
    }
    else
    {
      state_ = state::read;
      om |= fdopen_mode::in;
    }

    auto_fd fd;
    try
    {
      fd = fdopen (p, om);
    }
    catch (const io_error&)
    {
      bool c (state_ == state::write);

      diag_record dr (fail);
      dr << "unable to " << (c ? "create" : "open") << ' ' << p;

      if (c)
        dr << info << "did you forget to add fsdir{} prerequisite for "
           << "output directory?";

      dr << endf;
    }

    // Open the corresponding stream. Note that if we throw after that, the
    // corresponding member will not be destroyed. This is the reason for the
    // depdb/base split.
    //
    if (state_ == state::read)
    {
      new (&is_) ifdstream (move (fd), em);
      buf_ = static_cast<fdbuf*> (is_.rdbuf ());
    }
    else
    {
      new (&os_) ofdstream (move (fd), em);
      buf_ = static_cast<fdbuf*> (os_.rdbuf ());
    }
  }

  depdb::
  depdb (path_type&& p, timestamp mt)
      : depdb_base (p, mt),
        path (move (p)),
        mtime (mt != timestamp_nonexistent ? mt : timestamp_unknown),
        touch (false)
  {
    // Read/write the database format version.
    //
    if (state_ == state::read)
    {
      string* l (read ());
      if (l == nullptr || *l != "1")
        write ('1');
    }
    else
      write ('1');
  }

  depdb::
  depdb (path_type p)
      : depdb (move (p), file_mtime (p))
  {
  }

  void depdb::
  change (bool trunc)
  {
    assert (state_ != state::write);

    // Transfer the file descriptor from ifdstream to ofdstream. Note that the
    // steps in this dance must be carefully ordered to make sure we don't
    // call any destructors twice in the face of exceptions.
    //
    auto_fd fd (is_.release ());

    // Consider this scenario: we are overwriting an old line (so it ends with
    // a newline and the "end marker") but the operation failed half way
    // through. Now we have the prefix from the new line, the suffix from the
    // old, and everything looks valid. So what we need is to somehow
    // invalidate the old content so that it can never combine with (partial)
    // new content to form a valid line. One way to do that would be to
    // truncate the file.
    //
    if (trunc)
      fdtruncate (fd.get (), pos_);

    // Note: seek is required to switch from reading to writing.
    //
    fdseek (fd.get (), pos_, fdseek_mode::set);

    // @@ Strictly speaking, ofdstream can throw which will leave us in a
    //    non-destructible state. Unlikely but possible.
    //
    is_.~ifdstream ();
    new (&os_) ofdstream (move (fd),
                          ofdstream::badbit | ofdstream::failbit,
                          pos_);
    buf_ = static_cast<fdbuf*> (os_.rdbuf ());

    state_ = state::write;
    mtime = timestamp_unknown;
  }

  string* depdb::
  read_ ()
  {
    // Save the start position of this line so that we can overwrite it.
    //
    pos_ = buf_->tellg ();

    // Note that we intentionally check for eof after updating the write
    // position.
    //
    if (state_ == state::read_eof)
      return nullptr;

    getline (is_, line_); // Calls line_.erase().

    // The line should always end with a newline. If it doesn't, then this
    // line (and the rest of the database) is assumed corrupted. Also peek at
    // the character after the newline. We should either have the next line or
    // '\0', which is our "end marker", that is, it indicates the database
    // was properly closed.
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
    // write mode on this line. And, after calling read() again, write to the
    // next line (i.e., start from the "end marker").
    //
    if (c == '\0')
      state_ = state::read_eof;

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

    os_.write (s, static_cast<streamsize> (n));

    if (nl)
      os_.put ('\n');
  }

  void depdb::
  write (char c, bool nl)
  {
    // Switch to writing if we are still reading.
    //
    if (state_ != state::write)
      change ();

    os_.put (c);

    if (nl)
      os_.put ('\n');
  }

  void depdb::
  close ()
  {
    // If we are at eof, then it means all lines are good, there is the "end
    // marker" at the end, and we don't need to do anything, except, maybe
    // touch the file. Otherwise, if we are still in the read mode, truncate
    // the rest, and then add the "end marker" (we cannot have anything in the
    // write mode since we truncate in change()).
    //
    if (state_ == state::read_eof)
    {
      if (!touch)
      {
        is_.close ();
        return;
      }

      // While there are utime(2)/utimensat(2) (and probably something similar
      // for Windows), for now we just overwrite the "end marker". Hopefully
      // no implementation will be smart enough to recognize this is a no-op
      // and skip updating mtime (which would probably be incorrect, spec-
      // wise). And this could even be faster since we already have the file
      // descriptor. Or it might be slower since so far we've only been
      // reading.
      //
      change (false /* truncate */); // Write end marker below.
    }
    else if (state_ != state::write)
    {
      pos_ = buf_->tellg (); // The last line is accepted.
      change (true /* truncate */);
    }

#ifdef BUILD2_MTIME_CHECK
    start_ = system_clock::now ();
#endif

    os_.put ('\0'); // The "end marker".
    os_.close ();

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
    mtime = file_mtime (path); // Save for debugging/check below.
#endif
  }

#ifdef BUILD2_MTIME_CHECK
  void depdb::
  verify (const path_type& t, timestamp e)
  {
    if (state_ != state::write)
      return;

    // We could call the static version but then we would have lost additional
    // information for some platforms.
    //
    timestamp t_mt (file_mtime (t));
    timestamp d_mt (file_mtime (path));

    if (d_mt > t_mt)
    {
      if (e == timestamp_unknown)
        e = system_clock::now ();

      fail << "backwards modification times detected:\n"
           << start_ << " sequence start\n"
#if defined(__FreeBSD__)
           << mtime  << " close mtime\n"
#endif
           << d_mt   << " " << path.string () << '\n'
           << t_mt   << " " << t.string () << '\n'
           << e      << " sequence end";
    }
  }

  void depdb::
  verify (timestamp s, const path_type& d, const path_type& t, timestamp e)
  {
    timestamp t_mt (file_mtime (t));
    timestamp d_mt (file_mtime (d));

    if (d_mt > t_mt)
    {
      fail << "backwards modification times detected:\n"
           << s    << " sequence start\n"
           << d_mt << " " << d.string () << '\n'
           << t_mt << " " << t.string () << '\n'
           << e    << " sequence end";
    }
  }
#endif // BUILD2_MTIME_CHECK
}
