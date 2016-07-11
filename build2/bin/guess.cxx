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
    struct guess_result
    {
      string id;
      string signature;
      string checksum;

      bool
      empty () const {return id.empty ();}
    };

    ar_info
    guess_ar (const path& ar, const path& rl)
    {
      tracer trace ("bin::guess_ar");

      guess_result arr, rlr;

      // Binutils, LLVM, and FreeBSD ar/ranlib all recognize the --version
      // option. While Microsoft's lib.exe doesn't support --version, it only
      // issues a warning and exits with zero status, printing its usual
      // banner before that (running lib.exe without any options result in
      // non-zero exit status -- go figure). So we are going to start with
      // that.
      //
      {
        auto f = [] (string& l) -> guess_result
        {
          // Binutils ar --version output has a line that starts with
          // "GNU ar ".
          //
          if (l.compare (0, 7, "GNU ar ") == 0)
            return guess_result {"gnu", move (l), ""};

          // LLVM ar --version output has a line that starts with
          // "LLVM version ".
          //
          if (l.compare (0, 13, "LLVM version ") == 0)
            return guess_result {"llvm", move (l), ""};

          // FreeBSD ar --verison output starts with "BSD ar ".
          //
          if (l.compare (0, 7, "BSD ar ") == 0)
            return guess_result {"bsd", move (l), ""};

          // Microsoft lib.exe output starts with "Microsoft (R) ".
          //
          if (l.compare (0, 14, "Microsoft (R) ") == 0)
            return guess_result {"msvc", move (l), ""};

          return guess_result ();
        };

        // Suppress all the errors because we may be trying an unsupported
        // option. Note that in case of lib.exe we will hash the warning
        // (yes, it goes to stdout) but that seems harmless.
        //
        sha256 cs;
        arr = run<guess_result> (ar, "--version", f, false, false, &cs);

        if (!arr.empty ())
          arr.checksum = cs.string ();
      }

      // On Mac OS X (and probably also older BSDs) ar/ranlib doesn't have an
      // option to display version or help. If we run it without any arguments
      // it dumps usage and exist with an error status. So we will have to use
      // that.
      //
      if (arr.empty ())
      {
        auto f = [] (string& l) -> guess_result
        {
          return l.find (" ar ") != string::npos
            ? guess_result {"generic", move (l), ""}
            : guess_result ();
        };

        // Redirect STDERR to STDOUT and ignore exit status.
        //
        sha256 cs;
        arr = run<guess_result> (ar, f, false, true, &cs);

        if (!arr.empty ())
        {
          l4 ([&]{trace << "generic ar '" << arr.signature << "'";});
          arr.checksum = cs.string ();
        }
      }

      if (arr.empty ())
        fail << "unable to guess " << ar << " signature";

      // Now repeat pretty much the same steps for ranlib if requested.
      //
      if (!rl.empty ())
      {
        // Binutils, LLVM, and FreeBSD.
        //
        {
          auto f = [] (string& l) -> guess_result
          {
            // "GNU ranlib ".
            //
            if (l.compare (0, 11, "GNU ranlib ") == 0)
              return guess_result {"gnu", move (l), ""};

            // "LLVM version ".
            //
            if (l.compare (0, 13, "LLVM version ") == 0)
              return guess_result {"llvm", move (l), ""};

            // On FreeBSD we get "ranlib" rather than "BSD ranlib" for some
            // reason. Which means we can't really call it 'bsd' for sure.
            //
            //if (l.compare (0, 7, "ranlib ") == 0)
            //  return guess_result {"bsd", move (l), ""};

            return guess_result ();
          };

          sha256 cs;
          rlr = run<guess_result> (rl, "--version", f, false, false, &cs);

          if (!rlr.empty ())
            rlr.checksum = cs.string ();
        }

        // Mac OS X (and probably also older BSDs).
        //
        if (rlr.empty ())
        {
          auto f = [] (string& l) -> guess_result
          {
            return l.find ("ranlib") != string::npos
              ? guess_result {"generic", move (l), ""}
              : guess_result ();
          };

          // Redirect STDERR to STDOUT and ignore exit status.
          //
          sha256 cs;
          rlr = run<guess_result> (rl, f, false, true, &cs);

          if (!rlr.empty ())
          {
            l4 ([&]{trace << "generic ranlib '" << rlr.signature << "'";});
            rlr.checksum = cs.string ();
          }
        }

        if (rlr.empty ())
          fail << "unable to guess " << rl << " signature";
      }

      return ar_info {
        move (arr.id), move (arr.signature), move (arr.checksum),
        move (rlr.id), move (rlr.signature), move (rlr.checksum)};
    }

    ld_info
    guess_ld (const path& ld)
    {
      tracer trace ("bin::guess_ld");

      guess_result r;

      // Binutils ld recognizes the --version option. Microsoft's link.exe
      // doesn't support --version (nor any other way to get the version
      // without the error exist status) but it will still print its banner.
      // We also want to recognize link.exe as fast as possible since it will
      // be the most commonly configured linker (for other platoforms the
      // linker will normally be used indirectly via the compiler and the
      // bin.ld module won't be loaded). So we are going to ignore the error
      // exit status. Our signatures are fairly specific to avoid any kind
      // of false positives.
      //
      {
        auto f = [] (string& l) -> guess_result
        {
          // Microsoft link.exe output starts with "Microsoft (R) ".
          //
          if (l.compare (0, 14, "Microsoft (R) ") == 0)
            return guess_result {"msvc", move (l), ""};

          // Binutils ld.bfd --version output has a line that starts with
          // "GNU ld " while ld.gold -- "GNU gold".
          //
          if (l.compare (0, 7, "GNU ld ") == 0)
            return guess_result {"gnu", move (l), ""};

          if (l.compare (0, 9, "GNU gold ") == 0)
            return guess_result {"gold", move (l), ""};

          return guess_result ();
        };

        // Redirect STDERR to STDOUT and ignore exit status. Note that in case
        // of link.exe we will hash the diagnostics (yes, it goes to stdout)
        // but that seems harmless.
        //
        sha256 cs;
        r = run<guess_result> (ld, "--version", f, false, true, &cs);

        if (!r.empty ())
          r.checksum = cs.string ();
      }

      // Next try -v which will cover Apple's linkers.
      //
      if (r.empty ())
      {
        auto f = [] (string& l) -> guess_result
        {
          // New ld64 has "PROJECT:ld64" in the first line (output to stderr),
          // for example:
          //
          // @(#)PROGRAM:ld  PROJECT:ld64-242.2
          //
          if (l.find ("PROJECT:ld64") != string::npos)
            return guess_result {"ld64", move (l), ""};

          // Old ld has "cctools" in the first line, for example:
          //
          // Apple Computer, Inc. version cctools-622.9~2
          //
          if (l.find ("cctools") != string::npos)
            return guess_result {"cctools", move (l), ""};

          return guess_result ();
        };

        sha256 cs;
        r = run<guess_result> (ld, "-v", f, false, false, &cs);

        if (!r.empty ())
          r.checksum = cs.string ();
      }

      // Finally try -version which will take care of LLVM's lld.
      //
      if (r.empty ())
      {
        auto f = [] (string& l) -> guess_result
        {
          // Unlike other LLVM tools (e.g., ar), the lld's version is printed
          // (to stderr) as:
          //
          // LLVM Linker Version: 3.7
          //
          if (l.compare (0, 19, "LLVM Linker Version") == 0)
            return guess_result {"llvm", move (l), ""};

          return guess_result ();
        };

        // Suppress all the errors because we may be trying an unsupported
        // option.
        //
        sha256 cs;
        r = run<guess_result> (ld, "-version", f, false, false, &cs);

        if (!r.empty ())
          r.checksum = cs.string ();
      }

      if (r.empty ())
        fail << "unable to guess " << ld << " signature";

      return ld_info {move (r.id), move (r.signature), move (r.checksum)};
    }
  }
}
