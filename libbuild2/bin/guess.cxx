// file      : libbuild2/bin/guess.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bin/guess.hxx>

#include <libbuild2/diagnostics.hxx>

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

    // Search for a program first in paths if not NULL and then using the
    // standard path search semantics. Use var to suggest an override if the
    // search fails.
    //
    // Only search in PATH (specifically, omitting the current executable's
    // directory on Windows).
    //
    static process_path
    search (const path& prog, const char* paths, const char* var)
    {
      if (paths != nullptr)
      {
        process_path r (
          run_try_search (prog,
                          true        /* init */,
                          dir_path () /* fallback */,
                          true        /* path_only */,
                          paths));

        if (!r.empty ())
        {
          // Clear the recall path since we found it in custom search paths.
          // An alternative would have been to also do a search in PATH and if
          // the two effective paths are the same (which means, this program
          // is also in PATH), keep the recall. The benefit of this approach
          // is that we will have tidier command lines without long absolute
          // paths. The drawback is the extra complexity (we would need to
          // normalize the paths, etc). Let's keep it simple for now.
          //
          r.clear_recall ();
          return r;
        }
      }

      auto df = make_diag_frame (
        [var](const diag_record& dr)
        {
          dr << info << "use " << var << " to override";
        });

      return run_search (prog, true, dir_path (), true);
    }

    ar_info
    guess_ar (const path& ar, const path* rl, const char* paths)
    {
      tracer trace ("bin::guess_ar");

      guess_result arr, rlr;

      process_path arp (search (ar, paths, "config.bin.ar"));
      process_path rlp (rl != nullptr
                        ? search (*rl, paths, "config.bin.ranlib")
                        : process_path ());

      // Binutils, LLVM, and FreeBSD ar/ranlib all recognize the --version
      // option. While Microsoft's lib.exe doesn't support --version, it only
      // issues a warning and exits with zero status, printing its usual
      // banner before that (running lib.exe without any options result in
      // non-zero exit status -- go figure). So we are going to start with
      // that.
      //
      // LLVM's llvm-lib.exe is similar to the Microsoft's version except it
      // does not print any banners (it does print "LLVM Lib" phrase in the /?
      // output). In fact, there doesn't seem to be any way to extract its
      // version (maybe we could run llvm-ar instead -- it seems to be always
      // around).
      //
      {
        auto f = [&ar] (string& l, bool) -> guess_result
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

          // For now we will recognize LLVM lib via its name.
          //
          const string& s (ar.string ());
          size_t s_p (path::traits_type::find_leaf (s));
          size_t s_n (s.size ());

          if (find_stem (s, s_p, s_n, "llvm-lib") != string::npos)
            return guess_result ("msvc-llvm",
                                 "LLVM lib (unknown version)",
                                 semantic_version (0, 0, 0));

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
        auto f = [] (string& l, bool) -> guess_result
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
          auto f = [] (string& l, bool) -> guess_result
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
          auto f = [] (string& l, bool) -> guess_result
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
    guess_ld (const path& ld, const char* paths)
    {
      tracer trace ("bin::guess_ld");

      guess_result r;

      process_path pp (search (ld, paths, "config.bin.ld"));

      // Binutils ld recognizes the --version option. Microsoft's link.exe
      // doesn't support --version (nor any other way to get the version
      // without the error exit status) but it will still print its banner.
      // We also want to recognize link.exe as fast as possible since it will
      // be the most commonly configured linker (for other platforms the
      // linker will normally be used indirectly via the compiler and the
      // bin.ld module won't be loaded). So we are going to ignore the error
      // exit status. Our signatures are fairly specific to avoid any kind of
      // false positives.
      //
      // When it comes to LLD, ld.lld (Unix), lld-link (Windows), and wasm-ld
      // (WebAssembly) all recognize --version while ld64.lld (Mac OS) does
      // not (and not even -v per Apple ld64; LLVM bug #43721).
      //
      // Version extraction is a @@ TODO.
      //
      {
        auto f = [&ld] (string& l, bool) -> guess_result
        {
          string id;
          semantic_version ver;

          // Microsoft link.exe output starts with "Microsoft (R) ".
          //
          if (l.compare (0, 14, "Microsoft (R) ") == 0)
          {
            id = "msvc";
          }
          // LLD prints a line in the form "LLD X.Y.Z ...".
          //
          else if (l.compare (0, 4, "LLD ") == 0)
          {
            // The only way to distinguish between various LLD drivers is via
            // their name. Handle potential prefixes (say a target) and
            // suffixes (say a version).
            //
            const string& s (ld.string ());
            size_t s_p (path::traits_type::find_leaf (s));
            size_t s_n (s.size ());

            if      (find_stem (s, s_p, s_n, "ld.lld"  ) != string::npos)
              id = "gnu-lld";
            else if (find_stem (s, s_p, s_n, "lld-link") != string::npos)
              id = "msvc-lld";
            else if (find_stem (s, s_p, s_n, "ld64.lld") != string::npos)
              id = "ld64-lld";
            else if (find_stem (s, s_p, s_n, "wasm-ld" ) != string::npos)
              id = "wasm-lld";
          }
          // Binutils ld.bfd --version output has a line that starts with "GNU
          // ld " while ld.gold -- "GNU gold". Again, fortify it against
          // embedded toolchain customizations by search for "GNU " in the
          // former case (note that ld.lld mentions "GNU".
          //
          else if (l.compare (0, 9, "GNU gold ") == 0)
          {
            id = "gnu-gold";
          }
          else if (l.find ("GNU ") != string::npos)
          {
            id = "gnu";
          }

          return (id.empty ()
                  ? guess_result ()
                  : guess_result (move (id), move (l), move (ver)));
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
        auto f = [] (string& l, bool) -> guess_result
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

      // Finally try -version which will take care of older LLVM's lld.
      //
      if (r.empty ())
      {
        auto f = [] (string& l, bool) -> guess_result
        {
          // Unlike other LLVM tools (e.g., ar), the lld's version is printed
          // (to stderr) as:
          //
          // LLVM Linker Version: 3.7
          //
          if (l.compare (0, 19, "LLVM Linker Version") == 0)
            return guess_result ("gnu-lld", move (l), semantic_version ());

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
    guess_rc (const path& rc, const char* paths)
    {
      tracer trace ("bin::guess_rc");

      guess_result r;

      process_path pp (search (rc, paths, "config.bin.rc"));

      // Binutils windres recognizes the --version option.
      //
      // Version extraction is a @@ TODO.
      {
        auto f = [] (string& l, bool) -> guess_result
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
      // status. LLVM's llvm-rc.exe /? doesn't print any LLVM-identifyable
      // information (unlike llvm-lib.exe) and similarly there doesn't seem to
      // be any way to get its version.
      //
      if (r.empty ())
      {
        auto f = [&rc] (string& l, bool) -> guess_result
        {
          if (l.compare (0, 14, "Microsoft (R) ") == 0)
            return guess_result ("msvc", move (l), semantic_version ());

          // For now we will recognize LLVM rc via its name.
          //
          const string& s (rc.string ());
          size_t s_p (path::traits_type::find_leaf (s));
          size_t s_n (s.size ());

          if (find_stem (s, s_p, s_n, "llvm-rc") != string::npos)
            return guess_result ("msvc-llvm",
                                 "LLVM rc (unknown version)",
                                 semantic_version ());

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
