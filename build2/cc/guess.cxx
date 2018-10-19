// file      : build2/cc/guess.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/guess.hxx>

#include <map>
#include <cstring>  // strlen(), strchr()

#include <build2/diagnostics.hxx>

using namespace std;

namespace build2
{
  namespace cc
  {
    compiler_id::
    compiler_id (value_type v)
    {
      switch (v)
      {
      case clang_apple: type = "clang"; variant = "apple"; break;
      case clang:       type = "clang";                    break;
      case gcc:         type = "gcc";                      break;
      case msvc:        type = "msvc";                     break;
      case icc:         type = "icc";                      break;
      }
    }

    auto compiler_id::
    value () const -> value_type
    {
      if (type == "gcc")   return gcc;
      if (type == "clang") return variant.empty () ? clang : clang_apple;
      if (type == "msvc")  return msvc;
      if (type == "icc")   return icc;

      assert (false);
      return gcc;
    }

    string
    to_string (compiler_class cl)
    {
      switch (cl)
      {
      case compiler_class::gcc:  return "gcc";
      case compiler_class::msvc: return "msvc";
      }
      return string (); // Never reached.
    }

    // Standard library detection for GCC-class compilers.
    //
    // The src argument should detect the standard library based on the
    // preprocessor macros and output the result in the stdlib:="XXX" form.
    //
    static string
    stdlib (lang xl,
            const process_path& xp,
            const strings* c_po, const strings* x_po,
            const strings* c_co, const strings* x_co,
            const char* src)
    {
      cstrings args {xp.recall_string ()};
      if (c_po != nullptr) append_options (args, *c_po);
      if (x_po != nullptr) append_options (args, *x_po);
      if (c_co != nullptr) append_options (args, *c_co);
      if (x_co != nullptr) append_options (args, *x_co);
      args.push_back ("-x");
      switch (xl)
      {
      case lang::c:   args.push_back ("c");   break;
      case lang::cxx: args.push_back ("c++"); break;
      }
      args.push_back ("-E");
      args.push_back ("-");  // Read stdin.
      args.push_back (nullptr);

      // The source we are going to preprocess may contains #include's which
      // may fail to resolve if, for example, there is no standard library
      // (-nostdinc/-nostdinc++). So we are going to suppress diagnostics and
      // assume the error exit code means no standard library (of course it
      // could also be because there is something wrong with the compiler or
      // options but that we simply leave to blow up later).
      //
      process pr (run_start (3     /* verbosity */,
                             xp,
                             args.data (),
                             -1    /* stdin */,
                             -1    /* stdout */,
                             false /* error  */));
      string l, r;
      try
      {
        // Here we have to simultaneously write to stdin and read from stdout
        // with both operations having the potential to block. For now we
        // assume that src fits into the pipe's buffer.
        //
        ofdstream os (move (pr.out_fd));
        ifdstream is (move (pr.in_ofd),
                      fdstream_mode::skip,
                      ifdstream::badbit);

        os << src << endl;
        os.close ();

        while (!eof (getline (is, l)))
        {
          size_t p (l.find_first_not_of (' '));

          if (p != string::npos && l.compare (p, 9, "stdlib:=\"") == 0)
          {
            p += 9;
            r = string (l, p, l.size () - p - 1); // One for closing \".
            break;
          }
        }

        is.close ();
      }
      catch (const io_error&)
      {
        // Presumably the child process failed. Let run_finish() deal with
        // that.
      }

      if (!run_finish (args.data (), pr, false /* error */, l))
        r = "none";

      if (r.empty ())
        fail << "unable to determine " << xl << " standard library";

      return r;
    }

    // C standard library detection on POSIX (i.e., non-Windows) systems.
    // Notes:
    //
    // - We place platform macro-based checks (__FreeBSD__, __APPLE__, etc)
    //   after library macro-based ones in case a non-default libc is used.
    //
    static const char* c_stdlib_src =
"#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__ == 1                      \n"
"#  include <stddef.h>    /* Forces defining __KLIBC__ for klibc.        */ \n"
"#  include <limits.h>    /* Includes features.h for glibc.              */ \n"
"#  include <sys/types.h> /* Includes sys/cdefs.h for bionic.            */ \n"
"                         /* Includes sys/features.h for newlib.         */ \n"
"                         /* Includes features.h for uclibc.             */ \n"
"#    if defined(__KLIBC__)                                                 \n"
"     stdlib:=\"klibc\"                                                     \n"
"#  elif defined(__BIONIC__)                                                \n"
"     stdlib:=\"bionic\"                                                    \n"
"#  elif defined(__NEWLIB__)                                                \n"
"     stdlib:=\"newlib\"                                                    \n"
"#  elif defined(__UCLIBC__)                                                \n"
"     stdlib:=\"uclibc\"                                                    \n"
"#  elif defined(__dietlibc__) /* Also has to be defined manually by     */ \n"
"     stdlib:=\"dietlibc\"     /* or some wrapper.                       */ \n"
"#  elif defined(__MUSL__)     /* This libc refuses to define __MUSL__   */ \n"
"     stdlib:=\"musl\"         /* so it has to be defined by user.       */ \n"
"#  elif defined(__GLIBC__)    /* Check for glibc last since some libc's */ \n"
"     stdlib:=\"glibc\"        /* pretend to be it.                      */ \n"
"#  elif defined(__FreeBSD__)                                               \n"
"     stdlib:=\"freebsd\"                                                   \n"
"#  elif defined(__APPLE__)                                                 \n"
"     stdlib:=\"apple\"                                                     \n"
"#  else                                                                    \n"
"     stdlib:=\"other\"                                                     \n"
"#  endif                                                                   \n"
"#else                                                                      \n"
"  stdlib:=\"none\"                                                         \n"
"#endif                                                                     \n";

    // Pre-guess the compiler type based on the compiler executable name and
    // also return the start of that name in the path (used to derive the
    // toolchain pattern). Return empty string/npos if can't make a guess (for
    // example, because the compiler name is a generic 'c++'). Note that it
    // only guesses the type, not the variant.
    //
    static pair<string, size_t>
    pre_guess (lang xl, const path& xc)
    {
      tracer trace ("cc::pre_guess");

      // Analyze the last path component only.
      //
      const string& s (xc.string ());
      size_t s_p (path::traits::find_leaf (s));
      size_t s_n (s.size ());

      // Name separator characters (e.g., '-' in 'g++-4.8').
      //
      auto sep = [] (char c) -> bool
      {
        return c == '-' || c == '_' || c == '.';
      };

      auto stem = [&sep, &s, s_p, s_n] (const char* x) -> size_t
      {
        size_t m (strlen (x));
        size_t p (s.find (x, s_p, m));

        return (p != string::npos &&
                (      p == s_p || sep (s[p - 1])) && // Separated beginning.
                ((p + m) == s_n || sep (s[p + m])))   // Separated end.
        ? p
        : string::npos;
      };

      // Warn if the user specified a C compiler instead of C++ or vice versa.
      //
      lang o;                   // Other language.
      const char* as (nullptr); // Actual stem.
      const char* es (nullptr); // Expected stem.
      size_t p;                 // Executable name position.

      const size_t npos (string::npos);
      using pair = std::pair<string, size_t>;

      switch (xl)
      {
      case lang::c:
        {
          // Keep msvc last since 'cl' is very generic.
          //
          if ((p = stem ("gcc"))   != npos) return pair ("gcc",   p);
          if ((p = stem ("clang")) != npos) return pair ("clang", p);
          if ((p = stem ("icc"))   != npos) return pair ("icc",   p);
          if ((p = stem ("cl"))    != npos) return pair ("msvc",  p);

          if      (stem (as = "g++")     != npos) es = "gcc";
          else if (stem (as = "clang++") != npos) es = "clang";
          else if (stem (as = "icpc")    != npos) es = "icc";
          else if (stem (as = "c++")     != npos) es = "cc";

          o = lang::cxx;
          break;
        }
      case lang::cxx:
        {
          // Keep msvc last since 'cl' is very generic.
          //
          if ((p = stem ("g++"))     != npos) return pair ("gcc",   p);
          if ((p = stem ("clang++")) != npos) return pair ("clang", p);
          if ((p = stem ("icpc"))    != npos) return pair ("icc",   p);
          if ((p = stem ("cl"))      != npos) return pair ("msvc",  p);

          if      (stem (as = "gcc")   != npos) es = "g++";
          else if (stem (as = "clang") != npos) es = "clang++";
          else if (stem (as = "icc")   != npos) es = "icpc";
          else if (stem (as = "cc")    != npos) es = "c++";

          o = lang::c;
          break;
        }
      }

      if (es != nullptr)
        warn << xc << " looks like a " << o << " compiler" <<
          info << "should it be '" << es << "' instead of '" << as << "'?";

      l4 ([&]{trace << "unable to guess  compiler type of " << xc;});

      return pair ("", string::npos);
    }

    // Guess the compiler type and variant by running it. If the pre argument
    // is not empty, then only "confirm" the pre-guess. Return empty result if
    // unable to guess.
    //
    struct guess_result
    {
      compiler_id id;
      string signature;
      string checksum;
      process_path path;

      guess_result () = default;
      guess_result (compiler_id&& i, string&& s)
          : id (move (i)), signature (move (s)) {}

      bool
      empty () const {return id.empty ();}
    };

    // Allowed to change pre if succeeds.
    //
    static guess_result
    guess (lang, const string& xv, const path& xc, string& pre)
    {
      tracer trace ("cc::guess");

      guess_result r;

      process_path xp;
      {
        auto df = make_diag_frame (
          [&xv](const diag_record& dr)
          {
            dr << info << "use " << xv << " to override";
          });

        xp = run_search (xc, false /* init */); // Note: cached.
      }

      // Start with -v. This will cover gcc and clang.
      //
      // While icc also writes what may seem like something we can use to
      // detect it:
      //
      // icpc version 16.0.2 (gcc version 4.9.0 compatibility)
      //
      // That first word is actually the executable name. So if we rename
      // icpc to foocpc, we will get:
      //
      // foocpc version 16.0.2 (gcc version 4.9.0 compatibility)
      //
      // In fact, if someone renames icpc to g++, there will be no way for
      // us to detect this. Oh, well, their problem.
      //
      if (r.empty () && (pre.empty () || pre == "gcc" || pre == "clang"))
      {
        auto f = [] (string& l) -> guess_result
        {
          // The gcc/g++ -v output will have a line (currently last) in the
          // form:
          //
          // "gcc version X.Y.Z ..."
          //
          // The "version" word can probably be translated. For example:
          //
          // gcc version 3.4.4
          // gcc version 4.2.1
          // gcc version 4.8.2 (GCC)
          // gcc version 4.8.5 (Ubuntu 4.8.5-2ubuntu1~14.04.1)
          // gcc version 4.9.2 (Ubuntu 4.9.2-0ubuntu1~14.04)
          // gcc version 5.1.0 (Ubuntu 5.1.0-0ubuntu11~14.04.1)
          // gcc version 6.0.0 20160131 (experimental) (GCC)
          //
          if (l.compare (0, 4, "gcc ") == 0)
            return guess_result (compiler_id {"gcc", ""}, move (l));

          // The Apple clang/clang++ -v output will have a line (currently
          // first) in the form:
          //
          // "Apple (LLVM|clang) version X.Y.Z ..."
          //
          // Apple clang version 3.1 (tags/Apple/clang-318.0.58) (based on LLVM 3.1svn)
          // Apple clang version 4.0 (tags/Apple/clang-421.0.60) (based on LLVM 3.1svn)
          // Apple clang version 4.1 (tags/Apple/clang-421.11.66) (based on LLVM 3.1svn)
          // Apple LLVM version 4.2 (clang-425.0.28) (based on LLVM 3.2svn)
          // Apple LLVM version 5.0 (clang-500.2.79) (based on LLVM 3.3svn)
          // Apple LLVM version 5.1 (clang-503.0.40) (based on LLVM 3.4svn)
          // Apple LLVM version 6.0 (clang-600.0.57) (based on LLVM 3.5svn)
          // Apple LLVM version 6.1.0 (clang-602.0.53) (based on LLVM 3.6.0svn)
          // Apple LLVM version 7.0.0 (clang-700.0.53)
          // Apple LLVM version 7.0.0 (clang-700.1.76)
          // Apple LLVM version 7.0.2 (clang-700.1.81)
          // Apple LLVM version 7.3.0 (clang-703.0.16.1)
          //
          // Note that the gcc/g++ "aliases" for clang/clang++ also include
          // this line but it is (currently) preceded by "Configured with:
          // ...".
          //
          // Check for Apple clang before the vanilla one since the above line
          // also includes "clang".
          //
          if (l.compare (0, 6, "Apple ") == 0 &&
              (l.compare (6, 5, "LLVM ") == 0 ||
               l.compare (6, 6, "clang ") == 0))
            return guess_result (compiler_id {"clang", "apple"}, move (l));

          // The vanilla clang/clang++ -v output will have a line (currently
          // first) in the form:
          //
          // "[... ]clang version X.Y.Z[-...] ..."
          //
          // The "version" word can probably be translated. For example:
          //
          // FreeBSD clang version 3.4.1 (tags/RELEASE_34/dot1-final 208032) 20140512
          // Ubuntu clang version 3.5.0-4ubuntu2~trusty2 (tags/RELEASE_350/final) (based on LLVM 3.5.0)
          // Ubuntu clang version 3.6.0-2ubuntu1~trusty1 (tags/RELEASE_360/final) (based on LLVM 3.6.0)
          // clang version 3.7.0 (tags/RELEASE_370/final)
          //
          if (l.find ("clang ") != string::npos)
            return guess_result (compiler_id {"clang", ""}, move (l));

          return guess_result ();
        };

        // The -v output contains other information (such as the compiler
        // build configuration for gcc or the selected gcc installation for
        // clang) which makes sense to include into the compiler checksum. So
        // ask run() to calculate it for every line of the -v ouput.
        //
        // One notable consequence of this is that if the locale changes
        // (e.g., via LC_ALL), then the compiler signature will most likely
        // change as well because of the translated text.
        //
        sha256 cs;

        // Suppress all the compiler errors because we may be trying an
        // unsupported option.
        //
        r = run<guess_result> (3, xp, "-v", f, false, false, &cs);

        if (!r.empty ())
        {
          // If this is clang-apple and pre-guess was gcc then change it so
          // that we don't issue any warnings.
          //
          if (r.id.type == "clang" && r.id.variant == "apple" && pre == "gcc")
            pre = "clang";

          r.checksum = cs.string ();
        }
      }

      // Next try --version to detect icc.
      //
      if (r.empty () && (pre.empty () || pre == "icc"))
      {
        auto f = [] (string& l) -> guess_result
        {
          // The first line has the " (ICC) " in it, for example:
          //
          // icpc (ICC) 9.0 20060120
          // icpc (ICC) 11.1 20100414
          // icpc (ICC) 12.1.0 20110811
          // icpc (ICC) 14.0.0 20130728
          // icpc (ICC) 15.0.2 20150121
          // icpc (ICC) 16.0.2 20160204
          // icc (ICC) 16.0.2 20160204
          //
          if (l.find (" (ICC) ") != string::npos)
            return guess_result (compiler_id {"icc", ""}, move (l));

          return guess_result ();
        };

        r = run<guess_result> (3, xp, "--version", f, false);
      }

      // Finally try to run it without any options to detect msvc.
      //
      //
      if (r.empty () && (pre.empty () || pre == "msvc"))
      {
        auto f = [] (string& l) -> guess_result
        {
          // Check for "Microsoft (R)" and "C/C++" in the first line as a
          // signature since all other words/positions can be translated. For
          // example:
          //
          // Microsoft (R) 32-bit C/C++ Optimizing Compiler Version 13.10.6030 for 80x86
          // Microsoft (R) 32-bit C/C++ Optimizing Compiler Version 14.00.50727.762 for 80x86
          // Microsoft (R) 32-bit C/C++ Optimizing Compiler Version 15.00.30729.01 for 80x86
          // Compilador de optimizacion de C/C++ de Microsoft (R) version 16.00.30319.01 para x64
          // Microsoft (R) C/C++ Optimizing Compiler Version 17.00.50727.1 for x86
          // Microsoft (R) C/C++ Optimizing Compiler Version 18.00.21005.1 for x86
          // Microsoft (R) C/C++ Optimizing Compiler Version 19.00.23026 for x86
          // Microsoft (R) C/C++ Optimizing Compiler Version 19.10.24629 for x86
          //
          // In the recent versions the architecture is either "x86", "x64",
          // or "ARM".
          //
          if (l.find ("Microsoft (R)") != string::npos &&
              l.find ("C/C++") != string::npos)
            return guess_result (compiler_id {"msvc", ""}, move (l));

          return guess_result ();
        };

        // One can pass extra options/arguments to cl.exe with the CL and _CL_
        // environment variables. However, if such extra options are passed
        // without anything to compile, then cl.exe no longer prints usage and
        // exits successfully but instead issues an error and fails. So we are
        // going to unset these variables for our test (interestingly, only CL
        // seem to cause the problem but let's unset both, for good measure).
        //
        const char* env[] = {"CL=", "_CL_=", nullptr};

        r = run<guess_result> (3, process_env (xp, env), f, false);
      }

      if (!r.empty ())
      {
        if (!pre.empty () && r.id.type != pre)
        {
          l4 ([&]{trace << "compiler type guess mismatch"
                        << ", pre-guessed " << pre
                        << ", determined " << r.id.type;});

          r = guess_result ();
        }
        else
        {
          l5 ([&]{trace << xc << " is " << r.id << ": '"
                        << r.signature << "'";});

          r.path = move (xp);
        }
      }
      else
        l4 ([&]{trace << "unable to determine compiler type of " << xc;});

      return r;
    }

    // Try to derive the toolchain pattern.
    //
    // The s argument is the stem to look for in the leaf of the path. The ls
    // and rs arguments are the left/right separator characters. If either is
    // NULL, then the stem should be the prefix/suffix of the leaf,
    // respectively. Note that a path that is equal to stem is not considered
    // a pattern.
    //
    // Note that the default right separator includes digits to handle cases
    // like clang++37 (FreeBSD).
    //
    static string
    pattern (const path& xc,
             const char* s,
             const char* ls = "-_.",
             const char* rs = "-_.0123456789")
    {
      string r;
      size_t sn (strlen (s));

      if (xc.size () > sn)
      {
        string l (xc.leaf ().string ());
        size_t ln (l.size ());

        size_t b;
        if (ln >= sn && (b = l.find (s)) != string::npos)
        {
          // Check left separators.
          //
          if (b == 0 || (ls != nullptr && strchr (ls, l[b - 1]) != nullptr))
          {
            // Check right separators.
            //
            size_t e (b + sn);
            if (e == ln || (rs != nullptr && strchr (rs, l[e]) != nullptr))
            {
              l.replace (b, sn, "*", 1);
              path p (xc.directory ());
              p /= l;
              r = move (p).string ();
            }
          }
        }
      }

      return r;
    }


    static compiler_info
    guess_gcc (lang xl,
               const path& xc,
               const strings* c_po, const strings* x_po,
               const strings* c_co, const strings* x_co,
               const strings*, const strings*,
               guess_result&& gr)
    {
      tracer trace ("cc::guess_gcc");

      const process_path& xp (gr.path);

      // Extract the version. The signature line has the following format
      // though language words can be translated and even rearranged (see
      // examples above).
      //
      // "gcc version A.B.C[ ...]"
      //
      string& s (gr.signature);

      // Scan the string as words and look for one that looks like a version.
      //
      size_t b (0), e (0);
      while (next_word (s, b, e))
      {
        // The third argument to find_first_not_of() is the length of the
        // first argument, not the length of the interval to check. So to
        // limit it to [b, e) we are also going to compare the result to the
        // end of the word position (first space). In fact, we can just check
        // if it is >= e.
        //
        if (s.find_first_not_of ("1234567890.", b, 11) >= e)
          break;
      }

      if (b == e)
        fail << "unable to extract gcc version from '" << s << "'";

      compiler_version v;
      v.string.assign (s, b, string::npos);

      // Split the version into components.
      //
      size_t vb (b), ve (b);
      auto next = [&s, b, e, &vb, &ve] (const char* m) -> uint64_t
      {
        try
        {
          if (next_word (s, e, vb, ve, '.'))
            return stoull (string (s, vb, ve - vb));
        }
        catch (const invalid_argument&) {}
        catch (const out_of_range&) {}

        fail << "unable to extract gcc " << m << " version from '"
             << string (s, b, e - b) << "'" << endf;
      };

      v.major = next ("major");
      v.minor = next ("minor");
      v.patch = next ("patch");

      if (e != s.size ())
        v.build.assign (s, e + 1, string::npos);

      // Figure out the target architecture. This is actually a lot trickier
      // than one would have hoped.
      //
      // There is the -dumpmachine option but gcc doesn't adjust it per the
      // compile options (e.g., -m32). However, starting with 4.6 it has the
      // -print-multiarch option which gives (almost) the right answer. The
      // "almost" part has to do with it not honoring the -arch option (which
      // is really what this compiler is building for). To get to that, we
      // would have to resort to a hack like this:
      //
      // gcc -v -E - 2>&1 | grep cc1
      // .../cc1 ... -mtune=generic -march=x86-64
      //
      // Also, -print-multiarch will print am empty line if the compiler
      // actually wasn't built with multi-arch support.
      //
      // So for now this is what we are going to do for the time being: First
      // try -print-multiarch. If that works out (recent gcc configure with
      // multi-arch support), then use the result. Otherwise, fallback to
      // -dumpmachine (older gcc or not multi-arch).
      //
      cstrings args {xp.recall_string (), "-print-multiarch"};
      if (c_co != nullptr) append_options (args, *c_co);
      if (x_co != nullptr) append_options (args, *x_co);
      args.push_back (nullptr);

      // The output of both -print-multiarch and -dumpmachine is a single line
      // containing just the target triplet.
      //
      auto f = [] (string& l) {return move (l);};

      string t (run<string> (3, xp, args.data (), f, false));

      if (t.empty ())
      {
        l5 ([&]{trace << xc << " doesn's support -print-multiarch, "
                      << "falling back to -dumpmachine";});

        args[1] = "-dumpmachine";
        t = run<string> (3, xp, args.data (), f);
      }

      if (t.empty ())
        fail << "unable to extract target architecture from " << xc
             << " -print-multiarch or -dumpmachine output";

      string ot (t);

      // Parse the target into triplet (for further tests) ignoring any
      // failures.
      //
      target_triplet tt;
      try {tt = target_triplet (t);} catch (const invalid_argument&) {}

      // Derive the toolchain pattern. Try cc/c++ as a fallback.
      //
      string pat (pattern (xc, xl == lang::c ? "gcc" : "g++"));

      if (pat.empty ())
        pat = pattern (xc, xl == lang::c ? "cc" : "c++");

      // Runtime and standard library.
      //
      // GCC always uses libgcc (even on MinGW). Even with -nostdlib GCC's
      // documentation says that you should usually specify -lgcc.
      //
      string rt  ("libgcc");
      string csl (tt.system == "mingw32"
                  ? "msvc"
                  : stdlib (xl, xp, c_po, x_po, c_co, x_co, c_stdlib_src));
      string xsl;
      switch (xl)
      {
      case lang::c:   xsl = csl;     break;
      case lang::cxx:
        {
          // While GCC only supports it's own C++ standard library (libstdc++)
          // we still run the test to detect the "none" case (-nostdinc++).
          //
          const char* src =
            "#include <bits/c++config.h> \n"
            "stdlib:=\"libstdc++\"       \n";

          xsl = stdlib (xl, xp, c_po, x_po, c_co, x_co, src);
          break;
        }
      }

      return compiler_info {
        move (gr.path),
        move (gr.id),
        compiler_class::gcc,
        move (v),
        move (gr.signature),
        move (gr.checksum), // Calculated on whole -v output.
        move (t),
        move (ot),
        move (pat),
        "",
        move (rt),
        move (csl),
        move (xsl)};
    }

    static compiler_info
    guess_clang (lang xl,
                 const path& xc,
                 const strings* c_po, const strings* x_po,
                 const strings* c_co, const strings* x_co,
                 const strings* c_lo, const strings* x_lo,
                 guess_result&& gr)
    {
      const process_path& xp (gr.path);

      // Extract the version. Here we will try to handle both vanilla and
      // Apple clang since the signature lines are fairly similar. They have
      // the following format though language words can probably be translated
      // and even rearranged (see examples above).
      //
      // "[... ]clang version A.B.C[( |-)...]"
      // "Apple (clang|LLVM) version A.B[.C] ..."
      //
      string& s (gr.signature);

      // Some overrides for testing.
      //
      //s = "clang version 3.7.0 (tags/RELEASE_370/final)";
      //
      //gr.id.variant = "apple";
      //s = "Apple LLVM version 7.3.0 (clang-703.0.16.1)";
      //s = "Apple clang version 3.1 (tags/Apple/clang-318.0.58) (based on LLVM 3.1svn)";

      // Scan the string as words and look for one that looks like a version.
      // Use '-' as a second delimiter to handle versions like
      // "3.6.0-2ubuntu1~trusty1".
      //
      size_t b (0), e (0);
      while (next_word (s, b, e, ' ', '-'))
      {
        // The third argument to find_first_not_of() is the length of the
        // first argument, not the length of the interval to check. So to
        // limit it to [b, e) we are also going to compare the result to the
        // end of the word position (first space). In fact, we can just check
        // if it is >= e.
        //
        if (s.find_first_not_of ("1234567890.", b, 11) >= e)
          break;
      }

      if (b == e)
        fail << "unable to extract clang version from '" << s << "'";

      compiler_version v;
      v.string.assign (s, b, string::npos);

      // Split the version into components.
      //
      size_t vb (b), ve (b);
      auto next = [&s, b, e, &vb, &ve] (const char* m, bool opt) -> uint64_t
      {
        try
        {
          if (next_word (s, e, vb, ve, '.'))
            return stoull (string (s, vb, ve - vb));

          if (opt)
            return 0;
        }
        catch (const invalid_argument&) {}
        catch (const out_of_range&) {}

        fail << "unable to extract clang " << m << " version from '"
             << string (s, b, e - b) << "'" << endf;
      };

      v.major = next ("major", false);
      v.minor = next ("minor", false);
      v.patch = next ("patch", gr.id.variant == "apple");

      if (e != s.size ())
        v.build.assign (s, e + 1, string::npos);

      // Figure out the target architecture.
      //
      // Unlike gcc, clang doesn't have -print-multiarch. Its -dumpmachine,
      // however, respects the compile options (e.g., -m32).
      //
      cstrings args {xp.recall_string (), "-dumpmachine"};
      if (c_co != nullptr) append_options (args, *c_co);
      if (x_co != nullptr) append_options (args, *x_co);
      args.push_back (nullptr);

      // The output of -dumpmachine is a single line containing just the
      // target triplet.
      //
      auto f = [] (string& l) {return move (l);};
      string t (run<string> (3, xp, args.data (), f));

      if (t.empty ())
        fail << "unable to extract target architecture from " << xc
             << " -dumpmachine output";

      string ot (t);

      // Parse the target into triplet (for further tests) ignoring any
      // failures.
      //
      target_triplet tt;
      try {tt = target_triplet (t);} catch (const invalid_argument&) {}

      // For Clang on Windows targeting MSVC we remap the target to match
      // MSVC's.
      //
      if (tt.system == "windows-msvc")
      {
        // Keep the CPU and replace the rest.
        //
        // @@ Note that currently there is no straightforward way to determine
        // the VC version Clang is using. See:
        //
        // http://lists.llvm.org/pipermail/cfe-dev/2017-December/056240.html
        //
        tt.vendor = "microsoft";
        tt.system = "win32-msvc";
        tt.version = "14.1";
        t = tt.string ();
      }

      // Derive the toolchain pattern. Try clang/clang++, the gcc/g++ alias,
      // as well as cc/c++.
      //
      string pat (pattern (xc, xl == lang::c ? "clang" : "clang++"));

      if (pat.empty ())
        pat = pattern (xc, xl == lang::c ? "gcc" : "g++");

      if (pat.empty ())
        pat = pattern (xc, xl == lang::c ? "cc" : "c++");

      // Runtime and standard library.
      //
      // Clang can use libgcc, its own compiler-rt, or, on Windows targeting
      // MSVC, the VC's runtime. As usual, there is no straightforward way
      // to query this and silence on the mailing list. See:
      //
      // http://lists.llvm.org/pipermail/cfe-dev/2018-January/056494.html
      //
      // So for now we will just look for --rtlib (note: linker option) and if
      // none specified, assume some platform-specific defaults.
      //
      string rt;
      {
        auto find_rtlib = [] (const strings* ops) -> const string*
        {
          return ops != nullptr
          ? find_option_prefix ("--rtlib=", *ops, false)
          : nullptr;
        };

        const string* o;
        if ((o = find_rtlib (x_lo)) != nullptr ||
            (o = find_rtlib (c_lo)) != nullptr)
        {
          rt = string (*o, 8);
        }
        else if (tt.system == "win32-msvc")  rt = "msvc";
        else if (tt.system == "linux-gnu" ||
                 tt.system == "freebsd")     rt = "libgcc";
        else /* Mac OS, etc. */              rt = "compiler-rt";
      }

      string csl (tt.system == "win32-msvc" || tt.system == "mingw32"
                  ? "msvc"
                  : stdlib (xl, xp, c_po, x_po, c_co, x_co, c_stdlib_src));

      string xsl;
      switch (xl)
      {
      case lang::c:   xsl = csl; break;
      case lang::cxx:
        {
          // All Clang versions that we care to support have __has_include()
          // so we use it to determine which standard library is available.
          //
          // Note that we still include the corresponding headers to verify
          // things are usable. For the "other" case we include some
          // standard header to detect the "none" case (e.g, -nostdinc++).
          //
          const char* src =
            "#if __has_include(<__config>)           \n"
            "  #include <__config>                   \n"
            "  stdlib:=\"libc++\"                    \n"
            "#elif __has_include(<bits/c++config.h>) \n"
            "  #include <bits/c++config.h>           \n"
            "  stdlib:=\"libstdc++\"                 \n"
            "#else                                   \n"
            "  #include <cstddef>                    \n"
            "  stdlib:=\"other\"                     \n"
            "#endif                                  \n";

          xsl = tt.system == "win32-msvc"
            ? "msvcp"
            : stdlib (xl, xp, c_po, x_po, c_co, x_co, src);
          break;
        }
      }

      return compiler_info {
        move (gr.path),
        move (gr.id),
        compiler_class::gcc,
        move (v),
        move (gr.signature),
        move (gr.checksum), // Calculated on whole -v output.
        move (t),
        move (ot),
        move (pat),
        "",
        move (rt),
        move (csl),
        move (xsl)};
    }

    static compiler_info
    guess_icc (lang xl,
               const path& xc,
               const strings* c_po, const strings* x_po,
               const strings* c_co, const strings* x_co,
               const strings*, const strings*,
               guess_result&& gr)
    {
      const process_path& xp (gr.path);

      // Extract the version. If the version has the fourth component, then
      // the signature line (extracted with --version) won't include it. So we
      // will have to get a more elaborate line with -V. We will also have to
      // do it to get the compiler target that respects the -m option: icc
      // doesn't support -print-multiarch like gcc and its -dumpmachine
      // doesn't respect -m like clang. In fact, its -dumpmachine is
      // completely broken as it appears to print the compiler's host and not
      // the target (e.g., .../bin/ia32/icpc prints x86_64-linux-gnu).
      //
      // Some examples of the signature lines from -V output:
      //
      // Intel(R) C++ Compiler for 32-bit applications, Version 9.1 Build 20070215Z Package ID: l_cc_c_9.1.047
      // Intel(R) C++ Compiler for applications running on Intel(R) 64, Version 10.1 Build 20071116
      // Intel(R) C++ Compiler for applications running on IA-32, Version 10.1 Build 20071116 Package ID: l_cc_p_10.1.010
      // Intel C++ Intel 64 Compiler Professional for applications running on Intel 64, Version 11.0 Build 20081105 Package ID: l_cproc_p_11.0.074
      // Intel(R) C++ Intel(R) 64 Compiler Professional for applications running on Intel(R) 64, Version 11.1 Build 20091130 Package ID: l_cproc_p_11.1.064
      // Intel C++ Intel 64 Compiler XE for applications running on Intel 64, Version 12.0.4.191 Build 20110427
      // Intel(R) C++ Intel(R) 64 Compiler for applications running on Intel(R) 64, Version 16.0.2.181 Build 20160204
      // Intel(R) C++ Intel(R) 64 Compiler for applications running on IA-32, Version 16.0.2.181 Build 20160204
      // Intel(R) C++ Intel(R) 64 Compiler for applications running on Intel(R) MIC Architecture, Version 16.0.2.181 Build 20160204
      // Intel(R) C Intel(R) 64 Compiler for applications running on Intel(R) MIC Architecture, Version 16.0.2.181 Build 20160204
      //
      // We should probably also assume the language words can be translated
      // and even rearranged.
      //
      string& s (gr.signature);
      s.clear ();

      auto f = [] (string& l)
      {
        return l.compare (0, 5, "Intel") == 0 && (l[5] == '(' || l[5] == ' ')
        ? move (l)
        : string ();
      };

      // The -V output is sent to STDERR.
      //
      s = run<string> (3, xp, "-V", f, false);

      if (s.empty ())
        fail << "unable to extract signature from " << xc << " -V output";

      if (s.find (xl == lang::c ? " C " : " C++ ") == string::npos)
        fail << xc << " does not appear to be the Intel " << xl
             << " compiler" <<
          info << "extracted signature: '" << s << "'";

      // Scan the string as words and look for the version. It consist of only
      // digits and periods and contains at least one period.
      //

      // Some overrides for testing.
      //
      //s = "Intel(R) C++ Compiler for 32-bit applications, Version 9.1 Build 20070215Z Package ID: l_cc_c_9.1.047";
      //s = "Intel(R) C++ Compiler for applications running on Intel(R) 64, Version 10.1 Build 20071116";
      //s = "Intel(R) C++ Compiler for applications running on IA-32, Version 10.1 Build 20071116 Package ID: l_cc_p_10.1.010";
      //s = "Intel C++ Intel 64 Compiler Professional for applications running on Intel 64, Version 11.0 Build 20081105 Package ID: l_cproc_p_11.0.074";
      //s = "Intel(R) C++ Intel(R) 64 Compiler Professional for applications running on Intel(R) 64, Version 11.1 Build 20091130 Package ID: l_cproc_p_11.1.064";
      //s = "Intel C++ Intel 64 Compiler XE for applications running on Intel 64, Version 12.0.4.191 Build 20110427";

      size_t b (0), e (0), n;
      while (next_word (s, b, e, ' ', ',') != 0)
      {
        // The third argument to find_first_not_of() is the length of the
        // first argument, not the length of the interval to check. So to
        // limit it to [b, e) we are also going to compare the result to the
        // end of the word position (first space). In fact, we can just check
        // if it is >= e. Similar logic for find_first_of() except that we add
        // space to the list of character to make sure we don't go too far.
        //
        if (s.find_first_not_of ("1234567890.", b, 11) >= e &&
            s.find_first_of (". ", b, 2) < e)
          break;
      }

      if (b == e)
        fail << "unable to extract icc version from '" << s << "'";

      compiler_version v;
      v.string.assign (s, b, string::npos);

      // Split the version into components.
      //
      size_t vb (b), ve (b);
      auto next = [&s, b, e, &vb, &ve] (const char* m, bool opt) -> uint64_t
      {
        try
        {
          if (next_word (s, e, vb, ve, '.'))
            return stoull (string (s, vb, ve - vb));

          if (opt)
            return 0;
        }
        catch (const invalid_argument&) {}
        catch (const out_of_range&) {}

        fail << "unable to extract icc " << m << " version from '"
             << string (s, b, e - b) << "'" << endf;
      };

      v.major = next ("major", false);
      v.minor = next ("minor", false);
      v.patch = next ("patch", true);

      if (vb != ve && next_word (s, e, vb, ve, '.'))
        v.build.assign (s, vb, ve - vb);

      if (e != s.size ())
      {
        if (!v.build.empty ())
          v.build += ' ';

        v.build.append (s, e + 1, string::npos);
      }

      // Figure out the target CPU by re-running the compiler with -V and
      // compile options (which may include, e.g., -m32). The output will
      // contain two CPU keywords: the first is the host and the second is the
      // target (hopefully this won't get rearranged by the translation).
      //
      // The CPU keywords (based on the above samples) appear to be:
      //
      // "32-bit"
      // "IA-32"
      // "Intel"    "64"
      // "Intel(R)" "64"
      // "Intel(R)" "MIC"      (-dumpmachine says: x86_64-k1om-linux)
      //
      cstrings args {xp.recall_string (), "-V"};
      if (c_co != nullptr) append_options (args, *c_co);
      if (x_co != nullptr) append_options (args, *x_co);
      args.push_back (nullptr);

      // The -V output is sent to STDERR.
      //
      string t (run<string> (3, xp, args.data (), f, false));

      if (t.empty ())
        fail << "unable to extract target architecture from " << xc
             << " -V output";

      string arch;
      for (b = e = 0; (n = next_word (t, b, e, ' ', ',')) != 0; )
      {
        if (t.compare (b, n, "Intel(R)", 8) == 0 ||
            t.compare (b, n, "Intel", 5) == 0)
        {
          if ((n = next_word (t, b, e, ' ', ',')) != 0)
          {
            if (t.compare (b, n, "64", 2) == 0)
            {
              arch = "x86_64";
            }
            else if (t.compare (b, n, "MIC", 3) == 0)
            {
              arch = "x86_64"; // Plus "-k1om-linux" from -dumpmachine below.
            }
          }
          else
            break;
        }
        else if (t.compare (b, n, "IA-32", 5) == 0 ||
                 t.compare (b, n, "32-bit", 6) == 0)
        {
          arch = "i386";
        }
      }

      if (arch.empty ())
        fail << "unable to extract icc target architecture from '" << t << "'";

      // So we have the CPU but we still need the rest of the triplet. While
      // icc currently doesn't support cross-compilation (at least on Linux)
      // and we could have just used the build triplet (i.e., the architecture
      // on which we are running), who knows what will happen in the future.
      // So instead we are going to use -dumpmachine and substitute the CPU.
      //
      {
        auto f = [] (string& l) {return move (l);};
        t = run<string> (3, xp, "-dumpmachine", f);
      }

      if (t.empty ())
        fail << "unable to extract target architecture from " << xc
             << " -dumpmachine output";

      // The first component in the triplet is always CPU.
      //
      size_t p (t.find ('-'));

      if (p == string::npos)
        fail << "unable to parse icc target architecture '" << t << "'";

      t.swap (arch);
      t.append (arch, p, string::npos);

      string ot (t);

      // Parse the target into triplet (for further tests) ignoring any
      // failures.
      //
      target_triplet tt;
      try {tt = target_triplet (t);} catch (const invalid_argument&) {}

      // Derive the toolchain pattern.
      //
      string pat (pattern (xc, xl == lang::c ? "icc" : "icpc"));

      // Use the signature line to generate the checksum.
      //
      sha256 cs (s);

      // Runtime and standard library.
      //
      // For now we assume that unless it is Windows, we are targeting
      // Linux/GCC.
      //
      string rt  (tt.system == "win32-msvc" ? "msvc" : "libgcc");
      string csl (tt.system == "win32-msvc"
                  ? "msvc"
                  : stdlib (xl, xp, c_po, x_po, c_co, x_co, c_stdlib_src));
      string xsl;
      switch (xl)
      {
      case lang::c:   xsl = csl;     break;
      case lang::cxx:
        {
          xsl = tt.system == "win32-msvc" ? "msvcp" : "libstdc++";
          break;
        }
      }

      return compiler_info {
        move (gr.path),
        move (gr.id),
        compiler_class::gcc, //@@ TODO: msvc on Windows?
        move (v),
        move (gr.signature),
        cs.string (),
        move (t),
        move (ot),
        move (pat),
        "",
        move (rt),
        move (csl),
        move (xsl)};
    }

    static compiler_info
    guess_msvc (lang xl,
                const path& xc,
                const strings*, const strings*,
                const strings*, const strings*,
                const strings*, const strings*,
                guess_result&& gr)
    {
      // Extract the version. The signature line has the following format
      // though language words can be translated and even rearranged (see
      // examples above).
      //
      // "Microsoft (R) C/C++ Optimizing Compiler Version A.B.C[.D] for CPU"
      //
      // The CPU keywords (based on the above samples) appear to be:
      //
      // "80x86"
      // "x86"
      // "x64"
      // "ARM"
      //
      string& s (gr.signature);

      // Some overrides for testing.
      //
      //s = "Microsoft (R) 32-bit C/C++ Optimizing Compiler Version 15.00.30729.01 for 80x86";
      //s = "Compilador de optimizacion de C/C++ de Microsoft (R) version 16.00.30319.01 para x64";

      // Scan the string as words and look for the version. While doing this
      // also keep an eye on the CPU keywords.
      //
      string arch;
      size_t b (0), e (0);

      auto check_cpu = [&arch, &s, &b, &e] () -> bool
      {
        size_t n (e - b);

        if (s.compare (b, n, "x64", 3) == 0 ||
            s.compare (b, n, "x86", 3) == 0 ||
            s.compare (b, n, "ARM", 3) == 0 ||
            s.compare (b, n, "80x86", 5) == 0)
        {
          arch.assign (s, b, n);
          return true;
        }

        return false;
      };

      while (next_word (s, b, e, ' ', ','))
      {
        // First check for the CPU keywords in case in some language they come
        // before the version.
        //
        if (check_cpu ())
          continue;

        // The third argument to find_first_not_of() is the length of the
        // first argument, not the length of the interval to check. So to
        // limit it to [b, e) we are also going to compare the result to the
        // end of the word position (first space). In fact, we can just check
        // if it is >= e.
        //
        if (s.find_first_not_of ("1234567890.", b, 11) >= e)
          break;
      }

      if (b == e)
        fail << "unable to extract msvc version from '" << s << "'";

      compiler_version v;
      v.string.assign (s, b, e - b);

      // Split the version into components.
      //
      size_t vb (b), ve (b);
      auto next = [&s, b, e, &vb, &ve] (const char* m) -> uint64_t
      {
        try
        {
          if (next_word (s, e, vb, ve, '.'))
            return stoull (string (s, vb, ve - vb));
        }
        catch (const invalid_argument&) {}
        catch (const out_of_range&) {}

        fail << "unable to extract msvc " << m << " version from '"
             << string (s, b, e - b) << "'" << endf;
      };

      v.major = next ("major");
      v.minor = next ("minor");
      v.patch = next ("patch");

      if (next_word (s, e, vb, ve, '.'))
        v.build.assign (s, vb, ve - vb);

      // Continue scanning for the CPU.
      //
      if (e != s.size ())
      {
        while (next_word (s, b, e, ' ', ','))
        {
          if (check_cpu ())
            break;
        }
      }

      if (arch.empty ())
        fail << "unable to extract msvc target architecture from "
             << "'" << s << "'";

      // Now we need to map x86, x64, and ARM to the target triplets. The
      // problem is, there aren't any established ones so we got to invent
      // them ourselves. Based on the discussion in
      // <libbutl/target-triplet.mxx>, we need something in the
      // CPU-VENDOR-OS-ABI form.
      //
      // The CPU part is fairly straightforward with x86 mapped to 'i386' (or
      // maybe 'i686'), x64 to 'x86_64', and ARM to 'arm' (it could also
      // include the version, e.g., 'amrv8').
      //
      // The (toolchain) VENDOR is also straightforward: 'microsoft'. Why not
      // omit it? Two reasons: firstly, there are other compilers with the
      // otherwise same target, for example Intel C/C++, and it could be
      // useful to distinguish between them. Secondly, by having all four
      // components we remove any parsing ambiguity.
      //
      // OS-ABI is where things are not as clear cut. The OS part shouldn't
      // probably be just 'windows' since we have Win32 and WinCE. And WinRT.
      // And Universal Windows Platform (UWP). So perhaps the following values
      // for OS: 'win32', 'wince', 'winrt', 'winup'.
      //
      // For 'win32' the ABI part could signal the Microsoft C/C++ runtime by
      // calling it 'msvc'. And seeing that the runtimes are incompatible from
      // version to version, we should probably add the 'X.Y' version at the
      // end (so we essentially mimic the DLL name, e.g, msvcr120.dll).  Some
      // suggested we also encode the runtime type (those /M* options) though
      // I am not sure: the only "redistributable" runtime is multi-threaded
      // release DLL.
      //
      // The ABI part for the other OS values needs thinking. For 'winrt' and
      // 'winup' it probably makes sense to encode the WINAPI_FAMILY macro
      // value (perhaps also with the version). Some of its values:
      //
      // WINAPI_FAMILY_APP        Windows 10
      // WINAPI_FAMILY_PC_APP     Windows 8.1
      // WINAPI_FAMILY_PHONE_APP  Windows Phone 8.1
      //
      // For 'wince' we may also want to add the OS version, e.g., 'wince4.2'.
      //
      // Putting it all together, Visual Studio 2015 will then have the
      // following target triplets:
      //
      // x86  i386-microsoft-win32-msvc14.0
      // x64  x86_64-microsoft-win32-msvc14.0
      // ARM  arm-microsoft-winup-???
      //
      string t;

      if (arch == "ARM")
        fail << "cl.exe ARM/WinRT/UWP target is not yet supported";
      else
      {
        if (arch == "x64")
          t = "x86_64-microsoft-win32-msvc";
        else if (arch == "x86" || arch == "80x86")
          t = "i386-microsoft-win32-msvc";
        else
          assert (false);

        // Mapping of compiler versions to runtime versions:
        //
        // Note that VC15 has runtime version 14.1 but the DLLs are still
        // called *140.dll (they are said to be backwards-compatible).
        //
        // year   ver cl.exe  crt/dll
        //
        // 2017   15u7  19.14  14.1/140
        // 2017   15u6  19.13  14.1/140
        // 2017   15u5  19.12  14.1/140
        // 2017   15u3  19.11  14.1/140
        // 2017   15    19.10  14.1/140
        // 2015   14    19.00  14.0/140
        // 2013   12    18.00  12.0/120
        // 2012   11    17.00  11.0/110
        // 2010   10    16.00  10.0/100
        // 2008    9    15.00   9.0/90
        // 2005    8    14.00   8.0/80
        // 2003  7.1    13.10   7.1/71
        //
        /**/ if (v.major == 19 && v.minor >= 10) t += "14.1";
        else if (v.major == 19 && v.minor ==  0) t += "14.0";
        else if (v.major == 18 && v.minor ==  0) t += "12.0";
        else if (v.major == 17 && v.minor ==  0) t += "11.0";
        else if (v.major == 16 && v.minor ==  0) t += "10.0";
        else if (v.major == 15 && v.minor ==  0) t += "9.0";
        else if (v.major == 14 && v.minor ==  0) t += "8.0";
        else if (v.major == 13 && v.minor == 10) t += "7.1";
        else fail << "unable to map msvc compiler version '" << v.string
                  << "' to runtime version";
      }

      string ot (t);

      // Derive the toolchain pattern.
      //
      // If the compiler name is/starts with 'cl' (e.g., cl.exe, cl-14),
      // then replace it with '*' and use it as a pattern for lib, link,
      // etc.
      //
      string cpat (pattern (xc, "cl", nullptr, ".-"));
      string bpat (cpat); // Binutils pattern is the same as toolchain.

      // Use the signature line to generate the checksum.
      //
      sha256 cs (s);

      // Runtime and standard library.
      //
      string rt ("msvc");
      string csl ("msvc");
      string xsl;
      switch (xl)
      {
      case lang::c:   xsl = csl;     break;
      case lang::cxx: xsl = "msvcp"; break;
      }

      return compiler_info {
        move (gr.path),
        move (gr.id),
        compiler_class::msvc,
        move (v),
        move (gr.signature),
        cs.string (),
        move (t),
        move (ot),
        move (cpat),
        move (bpat),
        move (rt),
        move (csl),
        move (xsl)};
    }

    // Compiler checks can be expensive (we often need to run the compiler
    // several times) so we cache the result.
    //
    static map<string, compiler_info> cache;

    const compiler_info&
    guess (lang xl,
           const string& xv,
           const path& xc,
           const strings* c_po, const strings* x_po,
           const strings* c_co, const strings* x_co,
           const strings* c_lo, const strings* x_lo)
    {
      // First check the cache.
      //
      string key;
      {
        sha256 cs;
        cs.append (static_cast<size_t> (xl));
        cs.append (xc.string ());
        if (c_po != nullptr) hash_options (cs, *c_po);
        if (x_po != nullptr) hash_options (cs, *x_po);
        if (c_co != nullptr) hash_options (cs, *c_co);
        if (x_co != nullptr) hash_options (cs, *x_co);
        if (c_lo != nullptr) hash_options (cs, *c_lo);
        if (x_lo != nullptr) hash_options (cs, *x_lo);
        key = cs.string ();

        auto i (cache.find (key));
        if (i != cache.end ())
          return i->second;
      }

      pair<string, size_t> pre (pre_guess (xl, xc));
      string& type (pre.first);

      // If we could pre-guess the type based on the excutable name, then
      // try the test just for that compiler.
      //
      guess_result gr;

      if (!type.empty ())
      {
        gr = guess (xl, xv, xc, type);

        if (gr.empty ())
          warn << xc << " looks like " << type << " but it is not" <<
            info << "use " << xv << " to override";

        type.clear ();
      }

      if (gr.empty ())
        gr = guess (xl, xv, xc, type);

      if (gr.empty ())
        fail << "unable to guess " << xl << " compiler type of " << xc;

      compiler_info r;
      const compiler_id& id (gr.id);

      switch (id.value ())
      {
      case compiler_id::gcc:
        {
          assert (id.variant.empty ());
          r = guess_gcc (xl, xc,
                         c_po, x_po, c_co, x_co, c_lo, x_lo,
                         move (gr));
          break;
        }
      case compiler_id::clang:
      case compiler_id::clang_apple:
        {
          assert (id.variant.empty () || id.variant == "apple");
          r = guess_clang (xl, xc,
                           c_po, x_po, c_co, x_co, c_lo, x_lo,
                           move (gr));
          break;
        }
      case compiler_id::msvc:
        {
          assert (id.variant.empty ());
          r = guess_msvc (xl, xc,
                          c_po, x_po, c_co, x_co, c_lo, x_lo,
                          move (gr));
          break;
        }
      case compiler_id::icc:
        {
          assert (id.variant.empty ());
          r = guess_icc (xl, xc,
                         c_po, x_po, c_co, x_co, c_lo, x_lo,
                         move (gr));
          break;
        }
      }

      // Derive binutils pattern unless this has already been done by the
      // compiler-specific code.
      //

      // When cross-compiling the whole toolchain is normally prefixed with
      // the target triplet, e.g., x86_64-w64-mingw32-{gcc,g++,ar,ld}. But
      // oftentimes it is not quite canonical (and sometimes -- outright
      // bogus). So instead we are going to first try to derive the prefix
      // using the pre-guessed position of the compiler name. Note that we
      // still want to try the target in case we could not pre-guess (think
      // x86_64-w64-mingw32-c++).
      //
      // BTW, for GCC we also get gcc-{ar,ranlib} (but not -ld) which add
      // support for the LTO plugin though it seems more recent GNU binutils
      // (2.25) are able to load the plugin when needed automatically. So it
      // doesn't seem we should bother trying to support this on our end (one
      // way we could do it is by passing config.bin.{ar,ranlib} as hints).
      //
      // It's also normal for native (i.e., non-cross-compiler) builds of GCC
      // and Clang to not have binutils installed in the same directory and
      // instead relying on the system ones. In this case, if the compiler is
      // specified with the absolute path, the pattern will be the fallback
      // search directory (though it feels like it should be checked first
      // rather than last).
      //
      if (r.bin_pattern.empty ())
      {
        if (pre.second != 0 &&
            pre.second != string::npos &&
            !path::traits::is_separator (xc.string ()[pre.second - 1]))
        {
          r.bin_pattern.assign (xc.string (), 0, pre.second);
          r.bin_pattern += '*'; // '-' or similar is already there.
        }
      }

      if (r.bin_pattern.empty ())
      {
        const string& t (r.target);
        size_t n (t.size ());

        if (xc.size () > n + 1)
        {
          const string& l (xc.leaf ().string ());

          if (l.size () > n + 1 && l.compare (0, n, t) == 0 && l[n] == '-')
          {
            path p (xc.directory ());
            p /= t;
            p += "-*";
            r.bin_pattern = move (p).string ();
          }
        }
      }

      // If we could not derive the pattern, then see if we can come up with a
      // fallback search directory.
      //
      if (r.bin_pattern.empty ())
      {
        const path& p (r.path.recall.empty () ? xc : r.path.recall);

        if (!p.simple ())
          r.bin_pattern = p.directory ().representation (); // Trailing slash.
      }

      return (cache[key] = move (r));
    }

    path
    guess_default (lang xl, const string& c, const string& pat)
    {
      const char* s (nullptr);

      switch (xl)
      {
      case lang::c:
        {
          if      (c == "gcc")         s = "gcc";
          else if (c == "clang")       s = "clang";
          else if (c == "clang-apple") s = "clang";
          else if (c == "icc")         s = "icc";
          else if (c == "msvc")        s = "cl";

          break;
        }
      case lang::cxx:
        {
          if      (c == "gcc")         s = "g++";
          else if (c == "clang")       s = "clang++";
          else if (c == "clang-apple") s = "clang++";
          else if (c == "icc")         s = "icpc";
          else if (c == "msvc")        s = "cl";

          break;
        }
      }

      return path (apply_pattern (s, &pat));
    }
  }
}
