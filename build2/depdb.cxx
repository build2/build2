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
  depdb::
  depdb (path_type p)
      : path (move (p)), mtime (file_mtime (path)), touch (false)
  {
    fstream::openmode om (fstream::out | fstream::binary);
    fstream::iostate em (fstream::badbit);

    if (mtime == timestamp_nonexistent)
    {
      mtime = timestamp_unknown;
      state_ = state::write;
      em |= fstream::failbit;
    }
    else
    {
      state_ = state::read;
      om |= fstream::in;
    }

    fs_.open (path.string (), om);
    if (!fs_.is_open ())
    {
      bool c (state_ == state::write);

      diag_record dr (fail);
      dr << "unable to " << (c ? "create" : "open") << ' ' << path;

      if (c)
        dr << info << "did you forget to add fsdir{} prerequisite for "
           << "output directory?";
    }

    fs_.exceptions (em);

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

  void depdb::
  change (bool flush)
  {
    assert (state_ != state::write);

    fs_.clear ();
    fs_.exceptions (fstream::failbit | fstream::badbit);

    // Consider this scenario: we are overwriting an old line (so it ends with
    // a newline and the "end marker") but the operation failed half way
    // through. Now we have the prefix from the new line, the suffix from the
    // old, and everything looks valid. So what we need is to somehow
    // invalidate the old content so that it can never combine with (partial)
    // new content to form a valid line. One way would be to truncate the file
    // but that is not straightforward (see note in close()). Alternatively,
    // we can replace everything with the "end markers".
    //
    fs_.seekg (0, fstream::end);
    fstream::pos_type end (fs_.tellg ());

    if (end != pos_)
    {
      fs_.seekp (pos_);

      for (auto i (end - pos_); i != 0; --i)
        fs_.put ('\0');

      if (flush)
        fs_.flush ();
    }

    fs_.seekp (pos_); // Must be done when changing from read to write.

    state_ = state::write;
    mtime = timestamp_unknown;
  }

  string* depdb::
  read_ ()
  {
    // Save the start position of this line so that we can overwrite it.
    //
    pos_ = fs_.tellg ();

    // Note that we intentionally check for eof after updating the write
    // position.
    //
    if (state_ == state::read_eof)
      return nullptr;

    getline (fs_, line_); // Calls line_.erase().

    // The line should always end with a newline. If it doesn't, then this
    // line (and the rest of the database) is assumed corrupted. Also peek at
    // the character after the newline. We should either have the next line or
    // '\0', which is our "end marker", that is, it indicates the database
    // was properly closed.
    //
    fstream::int_type c;
    if (fs_.fail () || // Nothing got extracted.
        fs_.eof ()  || // Eof reached before delimiter.
        (c = fs_.peek ()) == fstream::traits_type::eof ())
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
    pos_ = fs_.tellg ();

    // Keep reading lines checking for the end marker after each newline.
    //
    fstream::int_type c;
    do
    {
      if ((c = fs_.get ()) == '\n')
      {
        if ((c = fs_.get ()) == '\0')
        {
          state_ = state::read_eof;
          return true;
        }
      }
    } while (c != fstream::traits_type::eof ());

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

    fs_.write (s, static_cast<streamsize> (n));

    if (nl)
      fs_.put ('\n');
  }

  void depdb::
  write (char c, bool nl)
  {
    // Switch to writing if we are still reading.
    //
    if (state_ != state::write)
      change ();

    fs_.put (c);

    if (nl)
      fs_.put ('\n');
  }

  void depdb::
  close ()
  {
    // If we are at eof, then it means all lines are good, there is the "end
    // marker" at the end, and we don't need to do anything, except, maybe
    // touch the file. Otherwise, we need to add the "end marker" and truncate
    // the rest.
    //
    if (state_ == state::read_eof)
    {
      // While there are utime(2)/utimensat(2) (and probably something similar
      // for Windows), for now we just overwrite the "end marker". Hopefully
      // no implementation will be smart enough to recognize this is a no-op
      // and skip updating mtime (which would probably be incorrect).
      //
      // It would be interesting to one day write an implementation that uses
      // POSIX file IO, futimens(), and ftruncate() and see how much better it
      // performs.
      //
      if (touch)
      {
        fs_.clear ();
        fs_.exceptions (fstream::failbit | fstream::badbit);
        fs_.seekp (0, fstream::cur); // Required to switch from read to write.
        state_ = state::write;       // Write end marker below.
      }
    }
    else
    {
      if (state_ != state::write)
      {
        pos_ = fs_.tellg (); // The last line is accepted.
        change (false); // Don't flush.
      }

      // Truncating an fstream is actually a non-portable pain in the butt.
      // What if we leave the junk after the "end marker"? These files are
      // pretty small and chances are they will occupy the filesystem's block
      // size (usually 4KB) whether they are truncated or not. So it might
      // actually be faster not to truncate.
    }

    // On some platforms (currently confirmed on Windows and FreeBSD, both
    // running as VMs) one can sometimes end up with a modification time that
    // is quite a bit after the call to close(). And this messes with our
    // arrangement that a valid depdb should be no older than the target it is
    // for.
    //
    // Note that this does not seem to be related to clock adjustments but
    // rather feels like the modification time is set when the changes
    // actually hit some lower-level layer (e.g., OS or filesystem
    // driver). One workaround that appears to work is to query the
    // mtime. This seems to force that layer to commit to a timestamp.
    //
    // Well, this seems to work on FreeBSD but on Windows we may still end up
    // getting the old mtime if we ask for it too soon. For such cases we are
    // going to just set it ourselves.
    //
#ifdef _WIN32
    timestamp mt (timestamp_unknown);

    // See libbutl/filesystem.cxx for details.
    //
    auto to_timestamp = [] (const FILETIME& t) -> timestamp
    {
      uint64_t nsec ((static_cast<uint64_t> (t.dwHighDateTime) << 32) |
                     t.dwLowDateTime);
      nsec -= 11644473600ULL * 10000000;
      nsec *= 100;

      return timestamp (
        chrono::duration_cast<duration> (chrono::nanoseconds (nsec)));
    };
#endif

    if (state_ == state::write)
    {
#ifdef BUILD2_MTIME_CHECK
      start_ = system_clock::now ();
#endif

      /*
#ifdef _WIN32
      //
      // On Windows there are two times, the precise time (which is what we
      // get with system_clock::now()) and what we will call "filesystem time"
      // which can lag the precise time by as much as couple of milliseconds.
      //
      FILETIME ft;
      GetSystemTimeAsFileTime (&ft);
      mt = to_timestamp (ft);

#ifdef BUILD2_MTIME_CHECK
      wtime_ = mt; // Save for check below.
#endif
#endif
      */

      fs_.put ('\0'); // The "end marker".
    }

    fs_.close ();

#if defined(_WIN32) || defined(__FreeBSD__)
    if (state_ == state::write)
    {
#ifdef _WIN32
      //
      // On Windows there are two times, the precise time (which is what we
      // get with system_clock::now()) and what we will call "filesystem time"
      // which can lag the precise time by as much as couple of milliseconds.
      //
      FILETIME ft;
      GetSystemTimeAsFileTime (&ft);
      mt = to_timestamp (ft);

#ifdef BUILD2_MTIME_CHECK
      wtime_ = mt; // Save for check below.
#endif

      auto file_mtime_h = [&to_timestamp] (const path_type& p) -> timestamp
      {
        HANDLE h (CreateFile (p.string ().c_str (),
                              FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL,
                              OPEN_EXISTING,
                              0,
                              NULL));

        assert (h != INVALID_HANDLE_VALUE);

        BY_HANDLE_FILE_INFORMATION fi;
        if (!GetFileInformationByHandle (h, &fi))
          assert (false);

        timestamp r (to_timestamp (fi.ftLastWriteTime));
        CloseHandle (h);
        return r;
      };

      mtime = file_mtime_h (path); // Save for check below.

      if (mtime < mt)
      {
        file_mtime (path, mt);
        assert (file_mtime_h (path) == mt);
      }
#else
      mtime = file_mtime (path); // Save for check below.
#endif
    }
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
#if defined(_WIN32)
           << wtime_ << " write time\n"
#endif
#if defined(_WIN32) || defined(__FreeBSD__)
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
