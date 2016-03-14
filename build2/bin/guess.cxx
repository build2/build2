// file      : build2/bin/guess.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/guess>

#include <build2/diagnostics>

using namespace std;

namespace build2
{
  namespace bin
  {
    bin_info
    guess (const path& ar, const path& ranlib)
    {
      tracer trace ("bin::guess");

      bin_info r;
      string& as (r.ar_signature);

      // Binutils, LLVM, and FreeBSD ar/ranlib all recognize the --version
      // option, so start with that.
      //
      {
        auto f = [] (string& l) -> string
        {
          // Binutils ar --version output has a line that starts with
          // "GNU ar ".
          //
          if (l.compare (0, 7, "GNU ar ") == 0)
            return move (l);

          // LLVM ar --version output has a line that starts with
          // "LLVM version ".
          //
          if (l.compare (0, 13, "LLVM version ") == 0)
            return move (l);

          // FreeBSD ar --verison output starts with "BSD ar ".
          //
          if (l.compare (0, 7, "BSD ar ") == 0)
            return move (l);

          return string ();
        };

        // Suppress all the errors because we may be trying an unsupported
        // option.
        //
        sha256 cs;
        as = run<string> (ar, "--version", f, false, false, &cs);

        if (!as.empty ())
          r.ar_checksum = cs.string ();
      }

      // On Mac OS X (and probably also older BSDs) ar/ranlib doesn't have an
      // option to display version or help. If we run it without any arguments
      // it dumps usage and exist with an error status. So we will have to use
      // that.
      //
      if (as.empty ())
      {
        auto f = [] (string& l) -> string
        {
          return l.find (" ar ") == string::npos ? string () : move (l);
        };

        // Redirect STDERR to STDOUT and ignore exit status.
        //
        sha256 cs;
        as = run<string> (ar, f, false, true, &cs);

        if (!as.empty ())
        {
          l4 ([&]{trace << "generic ar signature '" << as << "'";});

          r.ar_signature = "Generic ar";
          r.ar_checksum = cs.string ();
        }
      }

      if (as.empty ())
        fail << "unable to guess " << ar << " signature";

      // Now repeat pretty much the same steps for ranlib if requested.
      //
      if (ranlib.empty ())
        return r;

      string& rs (r.ranlib_signature);

      // Binutils, LLVM, and FreeBSD.
      //
      {
        auto f = [] (string& l) -> string
        {
          // "GNU ranlib ".
          //
          if (l.compare (0, 11, "GNU ranlib ") == 0)
            return move (l);

          // "LLVM version ".
          //
          if (l.compare (0, 13, "LLVM version ") == 0)
            return move (l);

          // "ranlib " (note: not "BSD ranlib " for some reason).
          //
          if (l.compare (0, 7, "ranlib ") == 0)
            return move (l);

          return string ();
        };

        sha256 cs;
        rs = run<string> (ranlib, "--version", f, false, false, &cs);

        if (!rs.empty ())
          r.ranlib_checksum = cs.string ();
      }

      // Mac OS X (and probably also older BSDs).
      //
      if (rs.empty ())
      {
        auto f = [] (string& l) -> string
        {
          return l.find ("ranlib") == string::npos ? string () : move (l);
        };

        // Redirect STDERR to STDOUT and ignore exit status.
        //
        sha256 cs;
        rs = run<string> (ranlib, f, false, true, &cs);

        if (!rs.empty ())
        {
          l4 ([&]{trace << "generic ranlib signature '" << rs << "'";});

          r.ranlib_signature = "Generic ranlib";
          r.ranlib_checksum = cs.string ();
        }
      }

      if (rs.empty ())
        fail << "unable to guess " << ranlib << " signature";

      return r;
    }
  }
}
