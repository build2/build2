// file      : build2/cxx/guess.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/guess>

#include <cstring>  // strlen()
#include <iostream> // cerr

#include <butl/sha256>
#include <butl/process>
#include <butl/fdstream>

#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
  {
    // Pre-guess the compiler type based on the compiler executable name.
    // Return empty string if can't make a guess (for example, because the
    // compiler name is a generic 'c++'). Note that it only guesses the type,
    // not the variant.
    //
    static string
    pre_guess (const path& cxx)
    {
      tracer trace ("cxx::pre_guess");

      const string s (cxx.leaf ().base ().string ());
      size_t n (s.size ());

      // Name separator characters (e.g., '-' in 'g++-4.8').
      //
      auto sep = [] (char c) -> bool
      {
        return c == '-' || c == '_' || c == '.';
      };

      auto stem = [&sep, &s, n] (const char* x) -> bool
      {
        size_t m (strlen (x));
        size_t p (s.find (x, 0, m));

        return p != string::npos &&
          (p == 0 || sep (s[p - 1])) &&  // Separated at the beginning.
          ((p += m) == n || sep (s[p])); // Separated at the end.
      };

      if (stem ("g++"))
        return "gcc";

      if (stem ("clang++"))
        return "clang";

      if (stem ("icpc"))
        return "icc";

      // Keep this one last since 'cl' is very generic.
      //
      if (stem ("cl"))
        return "msvc";

      // Warn if the user specified a C compiler instead of C++.
      //
      if (stem ("gcc"))
      {
        warn << cxx << " looks like a C compiler" <<
          info << "should it be 'g++' instead of 'gcc'?";
      }
      else if (stem ("clang"))
      {
        warn << cxx << " looks like a C compiler" <<
          info << "should it be 'clang++' instead of 'clang'?";
      }
      else if (stem ("icc"))
      {
        warn << cxx << " looks like a C compiler" <<
          info << "should it be 'icpc' instead of 'icc'?";
      }

      l4 ([&]{trace << "unable to guess compiler type of " << cxx;});
      return "";
    }

    // Start a process redirecting STDOUT and STDERR to a pipe.
    //
    static process
    start (const char* const* args)
    {
      if (verb >= 3)
        print_process (args);

      try
      {
        return process (args, 0, -1, 1);
      }
      catch (const process_error& e)
      {
        if (e.child ())
        {
          // Note: run() below relies on this exact message.
          //
          cerr << "unable to execute " << args[0] << ": " << e.what () << endl;
          exit (1);
        }
        else
          error << "unable to execute " << args[0] << ": " << e.what ();

        throw failed ();
      }
    };

    // Run the compiler with the specified option and then call the predicate
    // function on each line of the output until it returns a non-empty object
    // T (tested with T::empty()) which is then returned to the caller.
    //
    // The predicate can move the value out of the passed string but only in
    // case of a match (so that any diagnostics lines are left intact).
    //
    // If checksum is not NULL, then feed it the content of each line.
    //
    template <typename T>
    static T
    run (const char* const* args, T (*f) (string&), sha256* checksum = nullptr)
    try
    {
      process pr (start (args));
      ifdstream is (pr.in_ofd);

      T r;

      string l; // Last line of output.
      while (is.peek () != ifdstream::traits_type::eof () && getline (is, l))
      {
        trim (l);

        if (checksum != nullptr)
          checksum->append (l);

        if (r.empty ())
          r = f (l);
      }

      is.close (); // Don't block.

      if (!pr.wait ())
      {
        // While we want to suppress all the compiler errors because we may be
        // trying unsupported options, one error that we want to let through
        // is the inability to execute the compiler itself. We cannot reserve
        // a special exit status to signal this so we will just have to
        // compare the output. This particular situation will result in a
        // single error line printed by start() above.
        //
        if (l.compare (0, 18, "unable to execute ") == 0)
          fail << l;

        r = T ();
      }

      return r;
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e.what ();
      throw failed ();
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

      bool
      empty () const {return id.empty ();}
    };

    static guess_result
    guess (const path& cxx, const string& pre)
    try
    {
      tracer trace ("cxx::guess");

      guess_result r;
      const char* args[] = {cxx.string ().c_str (), nullptr, nullptr};

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
      if (r.id.empty () && (pre.empty () || pre == "gcc" || pre == "clang"))
      {
        auto f = [] (string& l) -> guess_result
        {
          // The g++ -v output will have a line (currently last) in the form:
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
            return guess_result {{"gcc", ""}, move (l), ""};

          // The Apple clang++ -v output will have a line (currently first)
          // in the form:
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
          // Note that the g++ "alias" for clang++ also includes this line
          // but it is (currently) preceded by "Configured with: ...".
          //
          // Check for Apple clang before the vanilla one since the above
          // line also includes "clang".
          //
          if (l.compare (0, 6, "Apple ") == 0 &&
              (l.compare (6, 5, "LLVM ") == 0 ||
               l.compare (6, 6, "clang ") == 0))
            return guess_result {{"clang", "apple"}, move (l), ""};

          // The vanilla clang++ -v output will have a line (currently first)
          // in the form:
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
            return guess_result {{"clang", ""}, move (l), ""};

          return guess_result ();
        };

        // The -v output contains other information (such as the compiler
        // build configuration for gcc or the selected gcc installation for
        // clang) which makes sense to include into the compiler checksum. So
        // ask run() to calculate it for every line of the -v ouput.
        //
        sha256 cs;

        args[1] = "-v";
        r = run<guess_result> (args, f, &cs);

        if (!r.empty ())
          r.checksum = cs.string ();
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
          //
          if (l.find (" (ICC) ") != string::npos)
            return guess_result {{"icc", ""}, move (l), ""};

          return guess_result ();
        };

        args[1] = "--version";
        r = run<guess_result> (args, f);
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
          //
          // In the recent versions the architecture is either "x86", "x64",
          // or "ARM".
          //
          if (l.find ("Microsoft (R)") != string::npos &&
              l.find ("C/C++") != string::npos)
            return guess_result {{"msvc", ""}, move (l), ""};

          return guess_result ();
        };

        args[1] = nullptr;
        r = run<guess_result> (args, f);
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
          l5 ([&]{trace << cxx << " is " << r.id << ": '"
                        << r.signature << "'";});
      }
      else
        l4 ([&]{trace << "unable to determine compiler type of " << cxx;});

      return r;
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << cxx << ": " << e.what ();
      throw failed ();
    }

    static compiler_info
    guess_gcc (const path& cxx,
               lookup<const value> coptions,
               guess_result&& gr)
    {
      tracer trace ("cxx::guess_gcc");

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

      // Split the version into components.
      //
      size_t vb (b), ve (b);
      auto next = [&s, b, e, &vb, &ve] (const char* m) -> string
      {
        if (!next_word (s, e, vb, ve, '.'))
          fail << "unable to extract gcc " << m << " version from '"
               << string (s, b, e - b) << "'";

        return string (s, vb, ve - vb);
      };

      compiler_version v;
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
      cstrings targs {cxx.string ().c_str (), "-print-multiarch"};
      append_options (targs, coptions);
      targs.push_back (nullptr);

      // The output of both -print-multiarch and -dumpmachine is a single line
      // containing just the target triplet.
      //
      auto f = [] (string& l) {return string (move (l));};

      string t (run<string> (targs.data (), f));

      if (t.empty ())
      {
        l5 ([&]{trace << cxx << " doesn's support -print-multiarch, "
                      << "falling back to -dumpmachine";});

        targs[1] = "-dumpmachine";
        t = run<string> (targs.data (), f);
      }

      if (t.empty ())
        fail << "unable to extract target architecture from " << cxx
             << " -print-multiarch or -dumpmachine output";

      return compiler_info {
        move (gr.id),
        move (v),
        move (gr.signature),
        move (gr.checksum), // Calculated on whole -v output.
        move (t)};
    }

    static compiler_info
    guess_clang (const path& cxx,
                 lookup<const value> coptions,
                 guess_result&& gr)
    {
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

      // Split the version into components.
      //
      size_t vb (b), ve (b);
      auto next = [&s, b, e, &vb, &ve] (const char* m) -> string
      {
        if (next_word (s, e, vb, ve, '.'))
          return string (s, vb, ve - vb);

        if (m != nullptr)
          fail << "unable to extract clang " << m << " version from '"
               << string (s, b, e - b) << "'";

        return string ();
      };

      compiler_version v;
      v.major = next ("major");
      v.minor = next ("minor");
      v.patch = next (gr.id.variant == "apple" ? nullptr : "patch");

      if (e != s.size ())
        v.build.assign (s, e + 1, string::npos);

      // Figure out the target architecture.
      //
      // Unlike gcc, clang doesn't have -print-multiarch. Its -dumpmachine,
      // however, respects the compile options (e.g., -m32).
      //
      cstrings targs {cxx.string ().c_str (), "-dumpmachine"};
      append_options (targs, coptions);
      targs.push_back (nullptr);

      // The output of -dumpmachine is a single line containing just the
      // target triplet.
      //
      auto f = [] (string& l) {return string (move (l));};

      string t (run<string> (targs.data (), f));

      if (t.empty ())
        fail << "unable to extract target architecture from " << cxx
             << " -dumpmachine output";

      return compiler_info {
        move (gr.id),
        move (v),
        move (gr.signature),
        move (gr.checksum), // Calculated on whole -v output.
        move (t)};
    }

    static compiler_info
    guess_icc (const path& cxx,
               lookup<const value> coptions,
               guess_result&& gr)
    {
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

      const char* vargs[] = {cxx.string ().c_str (), "-V", nullptr};
      s = run<string> (vargs, f);

      if (s.empty ())
        fail << "unable to extract signature from " << cxx << " -V output";

      if (s.find ("C++") == string::npos)
        fail << cxx << " does not appear to be the Intel C++ compiler" <<
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

      // Split the version into components.
      //
      size_t vb (b), ve (b);
      auto next = [&s, b, e, &vb, &ve] (const char* m) -> string
      {
        if (next_word (s, e, vb, ve, '.'))
          return string (s, vb, ve - vb);

        if (m != nullptr)
          fail << "unable to extract icc " << m << " version from '"
               << string (s, b, e - b) << "'";

        return "";
      };

      compiler_version v;
      v.major = next ("major");
      v.minor = next ("minor");
      v.patch = next (nullptr);
      if (!v.patch.empty ())
        v.build = next (nullptr);

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
      cstrings targs {cxx.string ().c_str (), "-V"};
      append_options (targs, coptions);
      targs.push_back (nullptr);

      string t (run<string> (targs.data (), f));

      if (t.empty ())
        fail << "unable to extract target architecture from " << cxx
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
      vargs[1] = "-dumpmachine";
      t = run<string> (vargs, [] (string& l) {return string (move (l));});

      if (t.empty ())
        fail << "unable to extract target architecture from " << cxx
             << " -dumpmachine output";

      // The first component in the triplet is always CPU.
      //
      size_t p (t.find ('-'));

      if (p == string::npos)
        fail << "unable to parse icc target architecture '" << t << "'";

      arch.append (t, p, string::npos);

      // Use the signature line to generate the checksum.
      //
      sha256 cs (s);

      return compiler_info {
        move (gr.id),
        move (v),
        move (gr.signature),
        cs.string (),
        move (arch)};
    }

    static compiler_info
    guess_msvc (const path&, guess_result&& gr)
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

      // Split the version into components.
      //
      size_t vb (b), ve (b);
      auto next = [&s, b, e, &vb, &ve] (const char* m) -> string
      {
        if (next_word (s, e, vb, ve, '.'))
          return string (s, vb, ve - vb);

        if (m != nullptr)
          fail << "unable to extract msvc " << m << " version from '"
               << string (s, b, e - b) << "'";

        return "";
      };

      compiler_version v;
      v.major = next ("major");
      v.minor = next ("minor");
      v.patch = next ("patch");
      v.build = next (nullptr);

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
      // them ourselves. Based on the discussion in <butl/triplet>, we need
      // something in the CPU-VENDOR-OS-ABI form.
      //
      // The CPU part is fairly straightforward with x86 mapped to 'i386' (or
      // maybe 'i686'), x64 to 'x86_64', and ARM to 'arm' (it could also
      // include the version, e.g., 'amrv8').
      //
      // The (toolchain) VENDOR is also straightforward: 'microsoft'. Why not
      // omit it? Two reasons: firstly, there are other compilers with the
      // otherwise same target, for example Intel C++, and it could be useful
      // to distinguish between them. Secondly, by having all four components
      // we remove any parsing ambiguity.
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
      if (arch == "ARM")
        fail << "cl.exe ARM/WinRT/UWP target is not yet supported";
      else
      {
        if (arch == "x64")
          arch = "x86_64-microsoft-win32-msvc";
        else if (arch == "x86" || arch == "80x86")
          arch = "i386-microsoft-win32-msvc";
        else
          assert (false);

        // Mapping of compiler versions to runtime versions:
        //
        // 19.00  140/14.0  VS2015
        // 18.00  120/12.0  VS2013
        // 17.00  110/11.0  VS2012
        // 16.00  110/10.0  VS2010
        // 15.00   90/9.0   VS2008
        // 14.00   80/8.0   VS2005
        // 13.10   71/7.1   VS2003
        //
        /**/ if (v.major == "19" && v.minor == "00") arch += "14.0";
        else if (v.major == "18" && v.minor == "00") arch += "12.0";
        else if (v.major == "17" && v.minor == "00") arch += "11.0";
        else if (v.major == "16" && v.minor == "00") arch += "10.0";
        else if (v.major == "15" && v.minor == "00") arch += "9.0";
        else if (v.major == "14" && v.minor == "00") arch += "8.0";
        else if (v.major == "13" && v.minor == "10") arch += "7.1";
        else fail << "unable to map msvc compiler version '" << v.string ()
                  << "' to runtime version";
      }

      // Use the signature line to generate the checksum.
      //
      sha256 cs (s);

      return compiler_info {
        move (gr.id),
        move (v),
        move (gr.signature),
        cs.string (),
        move (arch)};
    }

    compiler_info
    guess (const path& cxx, lookup<const value> coptions)
    {
      string pre (pre_guess (cxx));
      guess_result gr;

      // If we could pre-guess the type based on the excutable name, then
      // try the test just for that compiler.
      //
      if (pre.empty ())
      {
        gr = guess (cxx, pre);

        if (gr.empty ())
          warn << cxx << " name looks like " << pre << " but it is not";
      }

      if (gr.empty ())
        gr = guess (cxx, "");

      if (gr.empty ())
        fail << "unable to guess C++ compiler type of " << cxx;

      const compiler_id& id (gr.id);

      if (id.type == "gcc")
      {
        assert (id.variant.empty ());
        return guess_gcc (cxx, coptions, move (gr));
      }
      else if (id.type == "clang")
      {
        assert (id.variant.empty () || id.variant == "apple");
        return guess_clang (cxx, coptions, move (gr));
      }
      else if (id.type == "icc")
      {
        assert (id.variant.empty ());
        return guess_icc (cxx, coptions, move (gr));
      }
      else if (id.type == "msvc")
      {
        assert (id.variant.empty ());
        return guess_msvc (cxx, move (gr));
      }
      else
      {
        assert (false);
        return compiler_info ();
      }
    }
  }
}
