// file      : build/timestamp.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/timestamp>

#include <unistd.h>    // stat
#include <sys/types.h> // stat
#include <sys/stat.h>  // stat

#include <time.h> // localtime, gmtime, strftime

#include <ostream>
#include <system_error>

using namespace std;

namespace build
{
  // Figuring out whether we have the nanoseconds in some form.
  //
  template <typename S>
  constexpr auto nsec (const S* s) -> decltype(s->st_mtim.tv_nsec)
  {
    return s->st_mtim.tv_nsec; // POSIX (GNU/Linux, Solaris).
  }

  template <typename S>
  constexpr auto nsec (const S* s) -> decltype(s->st_mtimespec.tv_nsec)
  {
    return s->st_mtimespec.tv_nsec; // MacOS X.
  }

  template <typename S>
  constexpr auto nsec (const S* s) -> decltype(s->st_mtime_n)
  {
    return s->st_mtime_n; // AIX 5.2 and later.
  }

  template <typename S>
  constexpr int nsec (...) {return 0;}

  timestamp
  path_mtime (const path& p)
  {
    struct stat s;
    if (stat (p.string ().c_str (), &s) != 0)
    {
      if (errno == ENOENT || errno == ENOTDIR)
        return timestamp_nonexistent;
      else
        throw system_error (errno, system_category ());
    }

    return system_clock::from_time_t (s.st_mtime) +
      chrono::duration_cast<duration> (
        chrono::nanoseconds (nsec<struct stat> (&s)));
  }

  ostream&
  operator<< (ostream& os, const timestamp& ts)
  {
    // @@ replace with put_time()
    //

    time_t t (system_clock::to_time_t (ts));

    if (t == 0)
      return os << "<nonexistent>";

    std::tm tm;
    if (localtime_r (&t, &tm) == nullptr)
      throw system_error (errno, system_category ());

    // If year is greater than 9999, we will overflow.
    //
    char buf[20]; // YYYY-MM-DD HH:MM:SS\0
    if (strftime (buf, sizeof (buf), "%Y-%m-%d %H:%M:%S", &tm) == 0)
      return os << "<beyond year 9999>";

    os << buf;

    using namespace chrono;

    timestamp sec (system_clock::from_time_t (t));
    nanoseconds ns (duration_cast<nanoseconds> (ts - sec));

    if (ns != nanoseconds::zero ())
    {
      os << '.';
      os.width (9);
      os.fill ('0');
      os << ns.count ();
    }

    return os;
  }

  ostream&
  operator<< (ostream& os, const duration& d)
  {
    // @@ replace with put_time()
    //

    timestamp ts; // Epoch.
    ts += d;

    time_t t (system_clock::to_time_t (ts));

    const char* fmt (nullptr);
    const char* unt ("nanoseconds");
    if (t >= 365 * 12 * 24 * 60 * 60)
    {
      fmt = "%Y-%m-%d %H:%M:%S";
      unt = "years";
    }
    else if (t >= 12 * 24 * 60* 60)
    {
      fmt = "%m-%d %H:%M:%S";
      unt = "months";
    }
    else if (t >= 24 * 60* 60)
    {
      fmt = "%d %H:%M:%S";
      unt = "days";
    }
    else if (t >= 60 * 60)
    {
      fmt = "%H:%M:%S";
      unt = "hours";
    }
    else if (t >= 60)
    {
      fmt = "%M:%S";
      unt = "minutes";
    }
    else if (t >= 1)
    {
      fmt = "%S";
      unt = "seconds";
    }

    if (fmt != nullptr)
    {
      std::tm tm;
      if (gmtime_r (&t, &tm) == nullptr)
        throw system_error (errno, system_category ());

      char buf[20]; // YYYY-MM-DD HH:MM:SS\0
      if (strftime (buf, sizeof (buf), fmt, &tm) == 0)
        return os << "<beyond 9999>";

      os << buf;
    }

    using namespace chrono;

    timestamp sec (system_clock::from_time_t (t));
    nanoseconds ns (duration_cast<nanoseconds> (ts - sec));

    if (ns != nanoseconds::zero ())
    {
      if (fmt != nullptr)
      {
        os << '.';
        os.width (9);
        os.fill ('0');
      }

      os << ns.count () << ' ' << unt;
    }
    else if (fmt == 0)
      os << '0';

    return os;
  }
}
