// file      : build2/bin/guess.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/guess.hxx>

#include <build2/diagnostics.hxx>

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
      semantic_version version;

      guess_result () = default;
      guess_result (string&& i, string&& s, semantic_version&& v)
          : id (move (i)), signature (move (s)), version (move (v)) {}

      bool
      empty () const {return id.empty ();}
    };

    // Try to parse a semantic-like version from the specified position.
    // Return 0-version if the version is invalid.
    //
    static inline semantic_version
    parse_version (const string& s, size_t p = 0, const char* bs = ".-+~ ")
    {
      optional<semantic_version> v (parse_semantic_version (s, p, bs));
      return v ? *v : semantic_version ();
    }

    ar_info
    guess_ar (const path& ar, const path* rl, const dir_path& fallback)
    {
      tracer trace ("bin::guess_ar");

      process_path arp, rlp;
      guess_result arr, rlr;

      {
        auto df = make_diag_frame (
          [](const diag_record& dr)
          {
            dr << info << "use config.bin.ar to override";
          });

        arp = run_search (ar, true, fallback);
      }

      if (rl != nullptr)
      {
        auto df = make_diag_frame (
          [](const diag_record& dr)
          {
            dr << info << "use config.bin.ranlib to override";
          });

        rlp = run_search (*rl, true, fallback);
      }

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
          // Normally GNU binutils ar --version output has a line that starts
          // with "GNU ar" and ends with the version. For example:
          //
          // "GNU ar (GNU Binutils) 2.26"
          // "GNU ar (GNU Binutils for Ubuntu) 2.26.1"
          //
          // However, some embedded toolchain makers customize this stuff in
          // all kinds of ways. For example:
          //
          // "ppc-vle-ar (HighTec Release HDP-v4.6.6.1-bosch-1.3-3c1e3bc) build on 2017-03-23 (GNU Binutils) 2.20"
          // "GNU ar version 2.13 (tricore) using BFD version 2.13 (2008-12-10)"
          //
          // So let's look for "GNU " and be prepared to find junk instead of
          // the version.
          //
          if (l.find ("GNU ") != string::npos)
          {
            semantic_version v (parse_version (l, l.rfind (' ') + 1));
            return guess_result ("gnu", move (l), move (v));
          }

          // LLVM ar --version output has a line that starts with
          // "LLVM version " and ends with the version, for example:
          //
          // "LLVM version 3.5.2"
          // "LLVM version 5.0.0"
          //
          if (l.compare (0, 13, "LLVM version ") == 0)
          {
            semantic_version v (parse_version (l, l.rfind (' ') + 1));
            return guess_result ("llvm", move (l), move (v));
          }

          // FreeBSD ar --verison output starts with "BSD ar " followed by
          // the version and some extra information, for example:
          //
          // "BSD ar 1.1.0 - libarchive 3.1.2"
          //
          // We will treat the extra information as the build component.
          //
          if (l.compare (0, 7, "BSD ar ") == 0)
          {
            semantic_version v (parse_version (l, 7));
            return guess_result ("bsd", move (l), move (v));
          }

          // Microsoft lib.exe output starts with "Microsoft (R) " and ends
          // with a four-component version, for example:
          //
          // "Microsoft (R) Library Manager Version 14.00.24215.1"
          // "Microsoft (R) Library Manager Version 14.14.26428.1"
          //
          if (l.compare (0, 14, "Microsoft (R) ") == 0)
          {
            semantic_version v (parse_version (l, l.rfind (' ') + 1));
            return guess_result ("msvc", move (l), move (v));
          }

          return guess_result ();
        };

        // Suppress all the errors because we may be trying an unsupported
        // option. Note that in case of lib.exe we will hash the warning
        // (yes, it goes to stdout) but that seems harmless.
        //
        sha256 cs;
        arr = run<guess_result> (3, arp, "--version", f, false, false, &cs);

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
            ? guess_result ("generic", move (l), semantic_version ())
            : guess_result ();
        };

        // Redirect STDERR to STDOUT and ignore exit status.
        //
        sha256 cs;
        arr = run<guess_result> (3, arp, f, false, true, &cs);

        if (!arr.empty ())
        {
          l4 ([&]{trace << "generic ar '" << arr.signature << "'";});
          arr.checksum = cs.string ();
        }
      }

      if (arr.empty ())
        fail << "unable to guess " << ar << " signature";

      // Now repeat pretty much the same steps for ranlib if requested. We
      // don't bother with the version assuming it is the same as for ar.
      //
      if (rl != nullptr)
      {
        // Binutils, LLVM, and FreeBSD.
        //
        {
          auto f = [] (string& l) -> guess_result
          {
            // The same story as with ar: normally starts with "GNU ranlib "
            // but can vary.
            //
            if (l.find ("GNU ") != string::npos)
              return guess_result ("gnu", move (l), semantic_version ());

            // "LLVM version ".
            //
            if (l.compare (0, 13, "LLVM version ") == 0)
              return guess_result ("llvm", move (l), semantic_version ());

            // On FreeBSD we get "ranlib" rather than "BSD ranlib" for some
            // reason. Which means we can't really call it 'bsd' for sure.
            //
            //if (l.compare (0, 7, "ranlib ") == 0)
            //  return guess_result ("bsd", move (l), semantic_version ());

            return guess_result ();
          };

          sha256 cs;
          rlr = run<guess_result> (3, rlp, "--version", f, false, false, &cs);

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
              ? guess_result ("generic", move (l), semantic_version ())
              : guess_result ();
          };

          // Redirect STDERR to STDOUT and ignore exit status.
          //
          sha256 cs;
          rlr = run<guess_result> (3, rlp, f, false, true, &cs);

          if (!rlr.empty ())
          {
            l4 ([&]{trace << "generic ranlib '" << rlr.signature << "'";});
            rlr.checksum = cs.string ();
          }
        }

        if (rlr.empty ())
          fail << "unable to guess " << *rl << " signature";
      }

      return ar_info {
        move (arp),
        move (arr.id),
        move (arr.signature),
        move (arr.checksum),
        move (arr.version),

        move (rlp),
        move (rlr.id),
        move (rlr.signature),
        move (rlr.checksum)};
    }

    ld_info
    guess_ld (const path& ld, const dir_path& fallback)
    {
      tracer trace ("bin::guess_ld");

      guess_result r;

      process_path pp;
      {
        auto df = make_diag_frame (
          [](const diag_record& dr)
          {
            dr << info << "use config.bin.ld to override";
          });

        pp = run_search (ld, true, fallback);
      }

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
      // Version extraction is a @@ TODO.
      //
      {
        auto f = [] (string& l) -> guess_result
        {
          // Microsoft link.exe output starts with "Microsoft (R) ".
          //
          if (l.compare (0, 14, "Microsoft (R) ") == 0)
            return guess_result ("msvc", move (l), semantic_version ());

          // Binutils ld.bfd --version output has a line that starts with
          // "GNU ld " while ld.gold -- "GNU gold". Again, fortify it against
          // embedded toolchain customizations by search for "GNU " in the
          // former case.
          //
          if (l.compare (0, 9, "GNU gold ") == 0)
            return guess_result ("gold", move (l), semantic_version ());

          if (l.find ("GNU ") != string::npos)
            return guess_result ("gnu", move (l), semantic_version ());

          return guess_result ();
        };

        // Redirect STDERR to STDOUT and ignore exit status. Note that in case
        // of link.exe we will hash the diagnostics (yes, it goes to stdout)
        // but that seems harmless.
        //
        sha256 cs;
        r = run<guess_result> (3, pp, "--version", f, false, true, &cs);

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
            return guess_result ("ld64", move (l), semantic_version ());

          // Old ld has "cctools" in the first line, for example:
          //
          // Apple Computer, Inc. version cctools-622.9~2
          //
          if (l.find ("cctools") != string::npos)
            return guess_result ("cctools", move (l), semantic_version ());

          return guess_result ();
        };

        sha256 cs;
        r = run<guess_result> (3, pp, "-v", f, false, false, &cs);

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
            return guess_result ("llvm", move (l), semantic_version ());

          return guess_result ();
        };

        // Suppress all the errors because we may be trying an unsupported
        // option.
        //
        sha256 cs;
        r = run<guess_result> (3, pp, "-version", f, false, false, &cs);

        if (!r.empty ())
          r.checksum = cs.string ();
      }

      if (r.empty ())
        fail << "unable to guess " << ld << " signature";

      return ld_info {
        move (pp), move (r.id), move (r.signature), move (r.checksum)};
    }

    rc_info
    guess_rc (const path& rc, const dir_path& fallback)
    {
      tracer trace ("bin::guess_rc");

      guess_result r;

      process_path pp;
      {
        auto df = make_diag_frame (
          [](const diag_record& dr)
          {
            dr << info << "use config.bin.rc to override";
          });

        pp = run_search (rc, true, fallback);
      }

      // Binutils windres recognizes the --version option.
      //
      // Version extraction is a @@ TODO.
      {
        auto f = [] (string& l) -> guess_result
        {
          // Binutils windres --version output has a line that starts with
          // "GNU windres " but search for "GNU ", similar to other tools.
          //
          if (l.find ("GNU ") != string::npos)
            return guess_result ("gnu", move (l), semantic_version ());

          return guess_result ();
        };

        // Suppress all the errors because we may be trying an unsupported
        // option.
        //
        sha256 cs;
        r = run<guess_result> (3, pp, "--version", f, false, false, &cs);

        if (!r.empty ())
          r.checksum = cs.string ();
      }

      // Microsoft rc.exe /? prints its standard banner and exits with zero
      // status.
      //
      if (r.empty ())
      {
        auto f = [] (string& l) -> guess_result
        {
          if (l.compare (0, 14, "Microsoft (R) ") == 0)
            return guess_result ("msvc", move (l), semantic_version ());

          return guess_result ();
        };

        sha256 cs;
        r = run<guess_result> (3, pp, "/?", f, false, false, &cs);

        if (!r.empty ())
          r.checksum = cs.string ();
      }

      if (r.empty ())
        fail << "unable to guess " << rc << " signature";

      return rc_info {
        move (pp), move (r.id), move (r.signature), move (r.checksum)};
    }
  }
}
