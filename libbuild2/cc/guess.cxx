// file      : libbuild2/cc/guess.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/guess.hxx>

// Bootstrap build is always performed in the VC's command prompt and thus
// doesn't require the VC search functionality.
//
#if defined(_WIN32) && !defined(BUILD2_BOOTSTRAP)
#  include <libbutl/win32-utility.hxx>

#  include <unknwn.h>   // IUnknown
#  include <stdlib.h>   // _MAX_PATH
#  include <oleauto.h>  // SysFreeString()
#  include <guiddef.h>  // CLSID, IID
#  include <objbase.h>  // CoInitializeEx(), CoCreateInstance(), etc.

// MinGW may lack some macro definitions used in msvc-setup.h (see below), so
// we provide them if that's the case.
//
#  ifndef MAXUINT
#    define MAXUINT UINT_MAX
#  endif

// MinGW's sal.h (Microsoft's Source Code Annotation Language) may not contain
// all the in/out annotation macros.
//
#  ifndef _In_z_
#    define _In_z_
#  endif

#  ifndef _In_opt_z_
#    define _In_opt_z_
#  endif

#  ifndef _Out_opt_
#    define _Out_opt_
#  endif

#  ifndef _Deref_out_opt_
#    define _Deref_out_opt_
#  endif

#  ifndef _Out_writes_to_
#    define _Out_writes_to_(X, Y)
#  endif

#  ifndef _Deref_out_range_
#    define _Deref_out_range_(X, Y)
#  endif

#  ifndef _Outptr_result_maybenull_
#    define _Outptr_result_maybenull_
#  endif

#  ifndef _Reserved_
#    define _Reserved_
#  endif

// API for enumerating Visual Studio setup instances and querying information
// about them (see the LICENSE file for details).
//
#  include <libbuild2/cc/msvc-setup.h>

#  include <libbuild2/filesystem.hxx>
#endif

#include <cstring> // strlen(), strchr(), strstr()

#include <libbuild2/diagnostics.hxx>

using namespace std;

namespace build2
{
  namespace cc
  {
    using std::to_string;

    string
    to_string (compiler_type t)
    {
      string r;

      switch (t)
      {
      case compiler_type::clang: r = "clang"; break;
      case compiler_type::gcc:   r = "gcc";   break;
      case compiler_type::msvc:  r = "msvc";  break;
      case compiler_type::icc:   r = "icc";   break;
      }

      return r;
    }

    compiler_id::
    compiler_id (const std::string& id)
    {
      using std::string;

      size_t p (id.find ('-'));

      if      (id.compare (0, p, "gcc"  ) == 0) type = compiler_type::gcc;
      else if (id.compare (0, p, "clang") == 0) type = compiler_type::clang;
      else if (id.compare (0, p, "msvc" ) == 0) type = compiler_type::msvc;
      else if (id.compare (0, p, "icc"  ) == 0) type = compiler_type::icc;
      else
        throw invalid_argument (
          "invalid compiler type '" + string (id, 0, p) + '\'');

      if (p != string::npos)
      {
        variant.assign (id, p + 1, string::npos);

        if (variant.empty ())
          throw invalid_argument ("empty compiler variant");
      }
    }

    string compiler_id::
    string () const
    {
      std::string r (to_string (type));

      if (!variant.empty ())
      {
        r += '-';
        r += variant;
      }

      return r;
    }

    string
    to_string (compiler_class c)
    {
      string r;

      switch (c)
      {
      case compiler_class::gcc:  r = "gcc";  break;
      case compiler_class::msvc: r = "msvc"; break;
      }

      return r;
    }

    // Standard library detection for GCC-class compilers.
    //
    // The src argument should detect the standard library based on the
    // preprocessor macros and output the result in the stdlib:="XXX" form.
    //
    static string
    stdlib (lang xl,
            const process_path& xp,
            const strings& x_mo,
            const strings* c_po, const strings* x_po,
            const strings* c_co, const strings* x_co,
            const char* src)
    {
      cstrings args {xp.recall_string ()};
      if (c_po != nullptr) append_options (args, *c_po);
      if (x_po != nullptr) append_options (args, *x_po);
      if (c_co != nullptr) append_options (args, *c_co);
      if (x_co != nullptr) append_options (args, *x_co);
      append_options (args, x_mo);
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
      process pr (run_start (3       /* verbosity */,
                             xp,
                             args,
                             -1      /* stdin */,
                             -1      /* stdout */,
                             1       /* stderr (to stdout) */));
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

      if (!run_finish_code (args.data (), pr, l, 2 /* verbosity */))
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
"#  elif defined(__NetBSD__)                                                \n"
"     stdlib:=\"netbsd\"                                                    \n"
"#  elif defined(__OpenBSD__)                                               \n"
"     stdlib:=\"openbsd\"                                                   \n"
"#  elif defined(__APPLE__)                                                 \n"
"     stdlib:=\"apple\"                                                     \n"
"#  elif defined(__EMSCRIPTEN__)                                            \n"
"     stdlib:=\"emscripten\"                                                \n"
"#  else                                                                    \n"
"     stdlib:=\"other\"                                                     \n"
"#  endif                                                                   \n"
"#else                                                                      \n"
"  stdlib:=\"none\"                                                         \n"
"#endif                                                                     \n";

    // Pre-guess the compiler type and optionally variant based on the
    // compiler executable name and also return the start of that name in the
    // path (used to derive the toolchain pattern). Return invalid type and
    // npos if can't make a guess (for example, because the compiler name is a
    // generic 'c++').
    //
    struct pre_guess_result
    {
      compiler_type    type;
      optional<string> variant;
      size_t           pos;
    };

    static inline ostream&
    operator<< (ostream& os, const pre_guess_result& r)
    {
      os << r.type;

      if (r.variant && !r.variant->empty ())
        os << '-' << *r.variant;

      return os;
    }

    static pre_guess_result
    pre_guess (lang xl, const path& xc, const optional<compiler_id>& xi)
    {
      tracer trace ("cc::pre_guess");

      // Analyze the last path component only.
      //
      const string& s (xc.string ());
      size_t s_p (path::traits_type::find_leaf (s));
      size_t s_n (s.size ());

      using type = compiler_type;

      // If the user specified the compiler id, then only check the stem for
      // that compiler.
      //
      auto check = [&xi, &s, s_p, s_n] (type t,
                                        const char* stem,
                                        const char* v = nullptr)
        -> optional<pre_guess_result>
      {
        if (!xi || (xi->type == t && (v == nullptr || xi->variant == v)))
        {
          size_t p (find_stem (s, s_p, s_n, stem));

          if (p != string::npos)
          {
            if (v == nullptr && xi)
              v = xi->variant.c_str ();

            return pre_guess_result {
              t,
              v != nullptr ? optional<string> (v) : nullopt,
              p};
          }
        }

        return nullopt;
      };

      // Warn if the user specified a C compiler instead of C++ or vice versa.
      //
      lang o;                   // Other language.
      const char* as (nullptr); // Actual stem.
      const char* es (nullptr); // Expected stem.

      switch (xl)
      {
      case lang::c:
        {
          // Try more specific variants first. Keep msvc last since 'cl' is
          // very generic.
          //
          if (auto r = check (type::msvc,  "clang-cl", "clang" )) return *r;
          if (auto r = check (type::clang, "clang"             )) return *r;
          if (auto r = check (type::gcc,   "gcc"               )) return *r;
          if (auto r = check (type::icc,   "icc"               )) return *r;
          if (auto r = check (type::clang, "emcc", "emscripten")) return *r;
          if (auto r = check (type::msvc,  "cl"                )) return *r;

          if      (check (type::clang, as = "clang++")) es = "clang";
          else if (check (type::gcc,   as = "g++")    ) es = "gcc";
          else if (check (type::icc,   as = "icpc")   ) es = "icc";
          else if (check (type::clang, as = "em++")   ) es = "emcc";
          else if (check (type::msvc,  as = "c++")    ) es = "cc";

          o = lang::cxx;
          break;
        }
      case lang::cxx:
        {
          // Try more specific variants first. Keep msvc last since 'cl' is
          // very generic.
          //
          if (auto r = check (type::msvc,  "clang-cl", "clang" )) return *r;
          if (auto r = check (type::clang, "clang++"           )) return *r;
          if (auto r = check (type::gcc,   "g++"               )) return *r;
          if (auto r = check (type::icc,   "icpc"              )) return *r;
          if (auto r = check (type::clang, "em++", "emscripten")) return *r;
          if (auto r = check (type::msvc,  "cl"                )) return *r;

          if      (check (type::clang, as = "clang")) es = "clang++";
          else if (check (type::gcc,   as = "gcc")  ) es = "g++";
          else if (check (type::icc,   as = "icc")  ) es = "icpc";
          else if (check (type::clang, as = "emcc") ) es = "em++";
          else if (check (type::msvc,  as = "cc")   ) es = "c++";

          o = lang::c;
          break;
        }
      }

      if (es != nullptr)
        warn << xc << " looks like a " << o << " compiler" <<
          info << "should it be '" << es << "' instead of '" << as << "'?";

      // If the user specified the id, then continue as if we pre-guessed.
      //
      if (xi)
        return pre_guess_result {xi->type, xi->variant, string::npos};

      l4 ([&]{trace << "unable to guess compiler type of " << xc;});

      return pre_guess_result {invalid_compiler_type, nullopt, string::npos};
    }

    // Return the latest MSVC and Platform SDK installation information if
    // both are discovered on the system and nullopt otherwise. In particular,
    // don't fail on the underlying COM/OS errors returning nullopt instead.
    // This way a broken VC setup will be silently ignored.
    //
    // Note that Visual Studio versions prior to 15.0 are not supported.
    //
    // Note also the directories are absolute and normalized.
    //
    struct msvc_info
    {
      dir_path msvc_dir; // VC tools directory (...\Tools\MSVC\<ver>\).
      dir_path psdk_dir; // Platform SDK directory (...\Windows Kits\<ver>\).
      string   psdk_ver; // Platform SDK version (under Include/, Lib/, etc).
    };

#if defined(_WIN32) && !defined(BUILD2_BOOTSTRAP)

    static inline void
    msvc_info_deleter (void* p)
    {
      delete static_cast<msvc_info*> (p);
    }

    // We more or less follow the logic in the Clang 'simplementation (see
    // MSVC.cpp for details) but don't use the high level APIs (bstr_t,
    // com_ptr_t, etc) and the VC extensions (__uuidof(), class uuid
    // __declspecs, etc) that are poorly supported by MinGW GCC and Clang.
    //
    struct com_deleter
    {
      void operator() (IUnknown* p) const {if (p != nullptr) p->Release ();}
    };

    struct bstr_deleter
    {
      void operator() (BSTR p) const {if (p != nullptr) SysFreeString (p);}
    };

    // We don't use the __uuidof keyword (see above) and so define the
    // class/interface ids manually.
    //
    static const CLSID msvc_setup_config_clsid {
      0x177F0C4A, 0x1CD3, 0x4DE7,
      {0xA3, 0x2C, 0x71, 0xDB, 0xBB, 0x9F, 0xA3, 0x6D}};

    static const IID msvc_setup_config_iid {
      0x26AAB78C, 0x4A60, 0x49D6,
      {0xAF, 0x3B, 0x3C, 0x35, 0xBC, 0x93, 0x36, 0x5D}};

    static const IID msvc_setup_helper_iid {
      0x42B21B78, 0x6192, 0x463E,
      {0x87, 0xBF, 0xD5, 0x77, 0x83, 0x8F, 0x1D, 0x5C}};

    // If cl is not empty, then find an installation that contains this cl.exe
    // path. In this case the path must be absolute and normalized.
    //
    static optional<msvc_info>
    find_msvc (const path& cl = path ())
    {
      using namespace butl;

      assert (cl.empty () ||
              (cl.absolute () && cl.normalized (false /* sep */)));

      msvc_info r;

      // Try to obtain the MSVC directory.
      //
      {
        // Initialize the COM library for use by the current thread.
        //
        if (CoInitializeEx (nullptr /* pvReserved */,
                            COINIT_APARTMENTTHREADED) != S_OK)
          return nullopt;

        auto uninitializer (make_guard ([] () {CoUninitialize ();}));

        // Obtain the VS information retrieval interface. Failed that, assume
        // there is no VS installed.
        //
        unique_ptr<ISetupConfiguration2, com_deleter> sc;
        {
          ISetupConfiguration2* p;
          if (CoCreateInstance (msvc_setup_config_clsid,
                                nullptr /* pUnkOuter */,
                                CLSCTX_ALL,
                                msvc_setup_config_iid,
                                reinterpret_cast<LPVOID*> (&p)) != S_OK)
            return nullopt;

          sc.reset (p);
        }

        // Obtain the VS instance enumerator interface.
        //
        unique_ptr<IEnumSetupInstances, com_deleter> ei;
        {
          IEnumSetupInstances* p;
          if (sc->EnumAllInstances (&p) != S_OK)
            return nullopt;

          ei.reset (p);
        }

        // If we search for the latest VS then obtain an interface that helps
        // with the VS version parsing.
        //
        unique_ptr<ISetupHelper, com_deleter> sh;

        if (cl.empty ())
        {
          ISetupHelper* p;
          if (sc->QueryInterface (msvc_setup_helper_iid,
                                  reinterpret_cast<LPVOID*> (&p)) != S_OK)
            return nullopt;

          sh.reset (p);
        }

        using vs_ptr = unique_ptr<ISetupInstance, com_deleter>;

        // Return the Visual Studio instance VC directory path or the empty
        // path on error.
        //
        auto vc_dir = [] (const vs_ptr& vs)
        {
          // Note: we cannot use bstr_t due to the Clang 9.0 bug #42842.
          //
          BSTR p;
          if (vs->ResolvePath (L"VC", &p) != S_OK)
            return dir_path ();

          unique_ptr<wchar_t, bstr_deleter> deleter (p);

          // Convert BSTR to the NULL-terminated character string and then to
          // a path. Bail out if anything goes wrong.
          //
          dir_path r;

          try
          {
            int n (WideCharToMultiByte (CP_ACP,
                                        0       /* dwFlags */,
                                        p,
                                        -1,     /*cchWideChar */
                                        nullptr /* lpMultiByteStr */,
                                        0       /* cbMultiByte */,
                                        0       /* lpDefaultChar */,
                                        0       /* lpUsedDefaultChar */));

            if (n != 0) // Note: must include the terminating NULL character.
            {
              vector<char> ps (n);
              if (WideCharToMultiByte (CP_ACP,
                                       0,
                                       p, -1,
                                       ps.data (), n,
                                       0, 0) != 0)
                r = dir_path (ps.data ());
            }
          }
          catch (const invalid_path&) {}

          if (r.relative ()) // Also covers the empty directory case.
            return dir_path ();

          return r;
        };

        // Iterate over the VS instances and pick the latest or containing
        // cl.exe, if its path is specified. Bail out if any COM interface
        // function call fails.
        //
        vs_ptr vs;
        unsigned long long vs_ver (0); // VS version numeric representation.

        HRESULT hr;
        for (ISetupInstance* p;
             (hr = ei->Next (1, &p, nullptr /* pceltFetched */)) == S_OK; )
        {
          vs_ptr i (p);

          if (!cl.empty ())          // Searching for VS containing cl.exe.
          {
            dir_path d (vc_dir (i));
            if (d.empty ())
              return nullopt;

            if (cl.sub (d))
            {
              vs = move (i);
              r.msvc_dir = move (d); // Save not to query repeatedly.
              break;
            }
          }
          else                       // Searching for the latest VS.
          {
            BSTR iv; // For example, 16.3.29324.140.
            if (i->GetInstallationVersion (&iv) != S_OK)
              return nullopt;

            unique_ptr<wchar_t, bstr_deleter> deleter (iv);

            assert (sh != nullptr);

            unsigned long long v;
            if (sh->ParseVersion (iv, &v) != S_OK)
              return nullopt;

            if (vs == nullptr || v > vs_ver)
            {
              vs = move (i);
              vs_ver = v;
            }
          }
        }

        // Bail out if no VS instance is found or we didn't manage to iterate
        // through them successfully.
        //
        if (vs == nullptr || (hr != S_FALSE && hr != S_OK))
          return nullopt;

        // Note: we may already have the directory (search by cl.exe case).
        //
        if (r.msvc_dir.empty ())
        {
          assert (cl.empty ());

          r.msvc_dir = vc_dir (vs);

          if (r.msvc_dir.empty ())
            return nullopt;
        }

        // If cl.exe path is not specified, then deduce the default VC tools
        // directory for this Visual Studio instance. Otherwise, extract the
        // tools directory from this path.
        //
        // Note that in the latter case we could potentially avoid the above
        // iterating over the VS instances, but let's make sure that the
        // specified cl.exe path actually belongs to one of them as a sanity
        // check.
        //
        if (cl.empty ())
        {
          // Read the VC version from the file and bail out on error.
          //
          string vc_ver; // For example, 14.23.28105.

          path vp (
            r.msvc_dir /
            path ("Auxiliary\\Build\\Microsoft.VCToolsVersion.default.txt"));

          try
          {
            ifdstream is (vp);
            vc_ver = trim (is.read_text ());
          }
          catch (const io_error&) {}

          if (vc_ver.empty ())
            return nullopt;

          // Make sure that the VC version directory exists.
          //
          try
          {
            ((r.msvc_dir /= "Tools") /= "MSVC") /= vc_ver;

            if (!dir_exists (r.msvc_dir))
              return nullopt;
          }
          catch (const invalid_path&) {return nullopt;}
          catch (const system_error&) {return nullopt;}
        }
        else
        {
          (r.msvc_dir /= "Tools") /= "MSVC";

          // Extract the VC tools version from the cl.exe path and append it
          // to r.msvc_dir.
          //
          if (!cl.sub (r.msvc_dir))
            return nullopt;

          // For example, 14.23.28105\bin\Hostx64\x64\cl.exe.
          //
          path p (cl.leaf (r.msvc_dir)); // Can't throw.

          auto i (p.begin ()); // Tools version.
          if (i == p.end ())
            return nullopt;

          r.msvc_dir /= *i; // Can't throw.

          // For good measure, make sure that the tools version is not the
          // last component in the cl.exe path.
          //
          if (++i == p.end ())
            return nullopt;
        }
      }

      // Try to obtain the latest Platform SDK directory and version.
      //
      {
        // Read the Platform SDK directory path from the registry. Failed
        // that, assume there is no Platform SDK installed.
        //
        HKEY h;
        if (RegOpenKeyExA (
              HKEY_LOCAL_MACHINE,
              "SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots",
              0 /* ulOptions */,
              KEY_READ,
              &h) != ERROR_SUCCESS)
          return nullopt;

        DWORD t;

        // Reserve space for the terminating NULL character.
        //
        DWORD n (_MAX_PATH + 1);
        char buf[_MAX_PATH + 1];

        LSTATUS st (RegQueryValueExA (h,
                                      "KitsRoot10",
                                      nullptr,
                                      &t,
                                      reinterpret_cast<LPBYTE> (buf),
                                      &n));

        // Unlikely to fail, but we can't do much if that's the case.
        //
        RegCloseKey (h);

        // Note that the value length includes the terminating NULL character
        // and so cannot be zero.
        //
        if (st != ERROR_SUCCESS || t != REG_SZ || n == 0)
          return nullopt;

        try
        {
          r.psdk_dir = dir_path (buf);

          if (r.psdk_dir.relative ()) // Also covers the empty directory case.
            return nullopt;

          // Obtain the latest Platform SDK version as the lexicographically
          // greatest sub-directory name in the <psdk-dir>/Include directory.
          //
          for (const dir_entry& de:
                 dir_iterator (r.psdk_dir / dir_path ("Include"),
                               dir_iterator::no_follow))
          {
            if (de.type () == entry_type::directory)
            {
              const string& v (de.path ().string ());

              if (v.compare (0, 3, "10.") == 0 && v > r.psdk_ver)
                r.psdk_ver = v;
            }
          }
        }
        catch (const invalid_path&) {return nullopt;}
        catch (const system_error&) {return nullopt;}

        if (r.psdk_ver.empty ())
          return nullopt;
      }

      try
      {
        r.msvc_dir.normalize ();
        r.psdk_dir.normalize ();
      }
      catch (const invalid_path&)
      {
        return nullopt;
      }

      return r;
    }
#endif

    // Guess the compiler type and variant by running it. If the pre argument
    // is not empty, then only "confirm" the pre-guess. Return empty result if
    // unable to guess.
    //
    // If the compiler has both type and variant signatures (say, like
    // clang-emscripten), then the variant goes to signature and type goes to
    // type_signature. Otherwise, type_signature is not used.
    //
    struct guess_result
    {
      compiler_id id;
      string signature;
      string type_signature;
      string checksum;
      process_path path;

      // Optional additional information (for example, msvc_info).
      //
      static void
      null_info_deleter (void* p) { assert (p == nullptr); }

      using info_ptr = unique_ptr<void, void (*) (void*)>;

      info_ptr info = {nullptr, null_info_deleter};

      guess_result () = default;
      guess_result (compiler_id i, string&& s, string&& ts = {})
          : id (move (i)), signature (move (s)), type_signature (move (ts)) {}

      bool
      empty () const {return id.empty ();}
    };

    // Note: allowed to change pre if succeeds.
    //
    static guess_result
    guess (context& ctx,
           const char* xm,
           lang xl,
           const path& xc,
           const strings& x_mo,
           const optional<compiler_id>& xi,
           pre_guess_result& pre,
           sha256& cs)
    {
      tracer trace ("cc::guess");

      assert (!xi || (xi->type == pre.type && xi->variant == *pre.variant));

      using type = compiler_type;
      const type invalid = invalid_compiler_type;

      const type& pt (pre.type);
      const optional<string>& pv (pre.variant);

      using info_ptr = guess_result::info_ptr;
      guess_result r;

      process_path xp;
      info_ptr search_info (nullptr, guess_result::null_info_deleter);
      {
        auto df = make_diag_frame (
          [&xm](const diag_record& dr)
          {
            dr << info << "use config." << xm << " to override";
          });

        // Normally we just search in PATH but in some situations we may need
        // to fallback to an ad hoc search method. And the tricky question in
        // this case is what should the recall path be. It's natural to make
        // it the same as effective (which happens automatically if we use the
        // fallback directory mechanism of run_search()) so that any command
        // lines that we print are re-runnable by the user.
        //
        // On the other hand, passing the effective path (which would normally
        // be absolute) to recursive instances of the build system (e.g., when
        // running tests) will inhibit the ad hoc search which may supply
        // other parts of the "environment" necessary to use the compiler. The
        // good example of this is MSVC cl.exe which doesn't have any default
        // header/library search paths (and which are normally supplied by the
        // INCLUDE/LIB environment variables or explicitly via the command
        // line).
        //
        // So a sensible strategy here would be to use the effective path if
        // that's all that's required for the compiler to function (as, for
        // example, is the case for Clang targeting MSVC) and use the initial
        // path otherwise, thus triggering the same ad hoc search in any
        // recursive instances.
        //
        // The main drawback of the latter, of course, is that the commands we
        // print are no longer re-runnable (even though we may have supplied
        // the rest of the "environment" explicitly on the command line). Plus
        // we would need to save whatever environment variables we used to
        // form the fallback path in case of hermetic configuration.
        //
        // An alternative strategy is to try and obtain the corresponding
        // "environment" in case of the effective (absolute) path similar to
        // how it is done in case of the ad hoc search.
        //
        dir_path fb; // Fallback search directory.

#ifdef _WIN32
        // If we are running in the Visual Studio command prompt, add the
        // potentially bundled Clang directory as a fallback (for some reason
        // the Visual Studio prompts don't add it to PATH themselves).
        //
        if (xc.simple () &&
            (pt == type::clang ||
             (pt == type::msvc && pv && *pv == "clang")))
        {
          if (optional<string> v = getenv ("VCINSTALLDIR"))
          {
            try
            {
              fb = ((dir_path (move (*v)) /= "Tools") /= "Llvm") /= "bin";
            }
            catch (const invalid_path&)
            {
              // Ignore it.
            }
          }
        }
#endif

        // Only search in PATH (specifically, omitting the current
        // executable's directory on Windows).
        //
        // Note that the process_path instance will be cached (as part of
        // compiler_info) so init is false.
        //
        xp = run_try_search (xc,
                             false /* init */,
                             fb,
                             true  /* path_only */);

#if defined(_WIN32) && !defined(BUILD2_BOOTSTRAP)
        // If we pre-guessed MSVC or Clang (including clang-cl) try the search
        // and if not found, try to locate the MSVC installation and fallback
        // on that.
        //
        if (xp.empty ())
        {
          if (xc.simple () &&
              (pt == type::clang ||
               (pt == type::msvc && (!pv || *pv == "clang"))))
          {
            if (optional<msvc_info> mi = find_msvc ())
            {
              try
              {
                if (pt == type::msvc && !pv)
                {
                  // With MSVC you get a compiler binary per target (i.e.,
                  // there is nothing like -m32/-m64 or /MACHINE). Targeting
                  // 64-bit seems like as good of a default as any.
                  //
                  fb = ((dir_path (mi->msvc_dir) /= "bin") /= "Hostx64") /=
                    "x64";

                  search_info = info_ptr (
                    new msvc_info (move (*mi)), msvc_info_deleter);
                }
                else
                {
                  // Get to ...\VC\Tools\ from ...\VC\Tools\MSVC\<ver>\.
                  //
                  fb = (dir_path (mi->msvc_dir) /= "..") /= "..";
                  fb.normalize ();
                  (fb /= "Llvm") /= "bin";

                  // Note that in this case we drop msvc_info and extract it
                  // directly from Clang later.
                }

                xp = run_try_search (xc, false, fb, true);
              }
              catch (const invalid_path&)
              {
                // Ignore it.
              }
            }
          }
        }
        else
        {
          // We try to find the matching installation only for MSVC (for Clang
          // we extract this information from the compiler).
          //
          if (xc.absolute () && (pt == type::msvc && !pv))
          {
            path cl (xc);    // Absolute but may not be normalized.
            cl.normalize (); // Can't throw since this is an existing path.

            if (optional<msvc_info> mi = find_msvc (cl))
            {
              search_info = info_ptr (
                new msvc_info (move (*mi)), msvc_info_deleter);
            }
          }
        }
#endif

        if (xp.empty ())
          run_search_fail (xc);
      }

      // Run the compiler with the specified option (-v, --version, etc; can
      // also be NULL) calling the specified function on each trimmed output
      // line (see build2::run() for details).
      //
      // Note that we suppress all the compiler errors because we may be
      // trying an unsupported option (but still consider the exit code).
      //
      cstrings args {xp.recall_string ()};
      append_options (args, x_mo);
      args.push_back (nullptr); // Placeholder for the option.
      args.push_back (nullptr);

      process_env env (xp);

      // For now let's assume that all the platforms other than Windows
      // recognize LC_ALL.
      //
#ifndef _WIN32
      const char* evars[] = {"LC_ALL=C", nullptr};
      env.vars = evars;
#endif

      string cache;
      auto run = [&ctx, &cs, &env, &args, &cache] (
        const char* o,
        auto&& f,
        bool checksum = false) -> guess_result
      {
        args[args.size () - 2] = o;
        cache.clear ();
        return build2::run<guess_result> (
          ctx,
          3                          /* verbosity */,
          env,
          args,
          forward<decltype (f)> (f),
          false                      /* error */,
          false                      /* ignore_exit */,
          checksum ? &cs : nullptr);
      };

      // Start with -v. This will cover gcc and clang (including clang-cl and
      // Emscripten clang).
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
      if (r.empty () && (pt == invalid     ||
                         pt == type::gcc   ||
                         pt == type::clang ||
                         (pt == type::msvc && pv && *pv == "clang")))
      {
        auto f = [&xi, &pt, &cache] (string& l, bool last) -> guess_result
        {
          if (xi)
          {
            //@@ TODO: what about type_signature? Or do we just assume that
            //   the variant version will be specified along with type
            //   version? Do we even have this ability?

            // The signature line is first in Clang and last in GCC.
            //
            return (xi->type != type::gcc || last
                    ? guess_result (*xi, move (l))
                    : guess_result ());
          }

          size_t p;

          // The gcc -v output will have a last line in the form:
          //
          // "gcc version X[.Y[.Z]][...] ..."
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
          // gcc version 9.3-win32 20200320 (GCC)
          // gcc version 10-win32 20220324 (GCC)
          //
          if (cache.empty ())
          {
            if (last && l.compare (0, 4, "gcc ") == 0)
              return guess_result (compiler_id {type::gcc, ""}, move (l));
          }

          // The Apple clang -v output will have a line (currently first) in
          // the form:
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
          // Apple clang version 12.0.0 (clang-1200.0.32.27)
          //
          // Note that the gcc/g++ "aliases" for clang/clang++ also include
          // this line but it is (currently) preceded by "Configured with:
          // ...".
          //
          // Check for Apple clang before the vanilla one since the above line
          // also includes "clang".
          //
          if (cache.empty ())
          {
            if (l.compare (0, 6, "Apple ") == 0 &&
                (l.compare (6, 5, "LLVM ") == 0 ||
                 l.compare (6, 6, "clang ") == 0))
              return guess_result (compiler_id {type::clang, "apple"}, move (l));
          }

          // Emscripten emcc -v prints its own version and the clang version,
          // for example:
          //
          // emcc (...) 2.0.8
          // clang version 12.0.0 (...)
          //
          // The order, however is not guaranteed (see Emscripten issue
          // #12654). So things are going to get hairy.
          //
          if (l.compare (0, 5, "emcc ") == 0)
          {
            if (cache.empty ())
            {
              // Cache the emcc line and continue in order to get the clang
              // line.
              //
              cache = move (l);
              return guess_result ();
            }
            else if (cache.find ("clang ") != string::npos)
            {
              return guess_result (compiler_id {type::clang, "emscripten"},
                                   move (l),
                                   move (cache));
            }
          }

          // The vanilla clang -v output will have a first line in the form:
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
          // The clang-cl output is exactly the same, which means the only way
          // to distinguish it is based on the executable name.
          //
          // We must also watch out for potential misdetections, for example:
          //
          // Configured with: ../gcc/configure CC=clang CXX=clang++ ...
          //
          if ((p = l.find ("clang ")) != string::npos &&
              (p == 0 || l[p - 1] == ' '))
          {
            if (cache.empty ())
            {
              // Cache the clang line and continue in order to get the variant
              // line, if any.
              //
              cache = move (l);
              return guess_result ();
            }
            else if (cache.compare (0, 5, "emcc ") == 0)
            {
              return guess_result (compiler_id {type::clang, "emscripten"},
                                   move (cache),
                                   move (l));
            }
          }

          if (last)
          {
            if (cache.find ("clang ") != string::npos)
            {
              return guess_result (pt == type::msvc
                                   ? compiler_id {type::msvc, "clang"}
                                   : compiler_id {type::clang, ""},
                                   move (cache));
            }
          }

          return guess_result ();
        };

        // The -v output contains other information (such as the compiler
        // build configuration for gcc or the selected gcc installation for
        // clang) which makes sense to include into the compiler checksum. So
        // ask run() to calculate it for every line of the -v ouput.
        //
        r = run ("-v", f, true /* checksum */);

        if (r.empty ())
        {
          if (xi)
          {
            // Fallback to --version below in case this GCC/Clang-like
            // compiler doesn't support -v.
            //
            //fail << "unable to obtain " << xc << " signature with -v";
          }

          cs.reset ();
        }
        else
        {
          // If this is clang-apple and pre-guess was gcc then change it so
          // that we don't issue any warnings.
          //
          if (r.id.type == type::clang &&
              r.id.variant == "apple"  &&
              pt == type::gcc)
          {
            pre.type = type::clang;
            pre.variant = "apple";
          }
        }
      }

      // Next try --version to detect icc. As well as obtain signature for
      // GCC/Clang-like compilers in case -v above didn't work.
      //
      if (r.empty () && (pt == invalid   ||
                         pt == type::icc ||
                         pt == type::gcc ||
                         pt == type::clang))
      {
        auto f = [&xi] (string& l, bool) -> guess_result
        {
          // Assume the first line is the signature.
          //
          if (xi)
            return guess_result (*xi, move (l));

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
            return guess_result (compiler_id {type::icc, ""}, move (l));

          return guess_result ();
        };

        r = run ("--version", f);

        if (r.empty ())
        {
          if (xi)
            fail << "unable to obtain " << xc << " signature with --version";
        }
      }

      // Finally try to run it without any options to detect msvc.
      //
      if (r.empty () && (pt == invalid ||
                         pt == type::msvc))
      {
        auto f = [&xi] (string& l, bool) -> guess_result
        {
          // Assume the first line is the signature.
          //
          if (xi)
            return guess_result (*xi, move (l));

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
            return guess_result (compiler_id {type::msvc, ""}, move (l));

          return guess_result ();
        };

        // One can pass extra options/arguments to cl.exe with the CL and _CL_
        // environment variables. However, if such extra options are passed
        // without anything to compile, then cl.exe no longer prints usage and
        // exits successfully but instead issues an error and fails. So we are
        // going to unset these variables for our test (interestingly, only CL
        // seem to cause the problem but let's unset both, for good measure).
        //
        // This is also the reason why we don't pass the mode options.
        //
        const char* evars[] = {"CL=", "_CL_=", nullptr};

        r = build2::run<guess_result> (ctx,
                                       3,
                                       process_env (xp, evars),
                                       f,
                                       false);

        if (r.empty ())
        {
          if (xi)
            fail << "unable to obtain " << xc << " signature";
        }
      }

      if (!r.empty ())
      {
        if (pt != invalid && (pt != r.id.type || (pv && *pv != r.id.variant)))
        {
          l4 ([&]{trace << "compiler type guess mismatch"
                        << ", pre-guessed " << pre
                        << ", determined " << r.id;});

          r = guess_result ();
        }
        else
        {
          l5 ([&]{trace << xc << " is " << r.id << ": '"
                        << r.signature << "'";});

          r.path = move (xp);

          if (search_info != nullptr && r.info == nullptr)
            r.info = move (search_info);
        }
      }
      else
        l4 ([&]{trace << "unable to determine compiler type of " << xc;});

      // Warn if the absolute compiler path looks like a ccache wrapper.
      //
      // The problem with ccache is that it pretends to be real GCC (i.e.,
      // it's --version output is indistinguishable from real GCC's) but does
      // not handle all valid GCC modes, in particular -fdirectives-only. As a
      // poor man's solution we check if the absolute compiler path contains
      // any mentioning of ccache (for example, /usr/lib64/ccache/g++ on
      // Fedora).
      //
      if (!r.empty ())
      {
        if (r.id.type == compiler_type::gcc ||
            r.id.type == compiler_type::clang)
        {
          if (strstr (r.path.effect_string (), "ccache") != nullptr)
            warn << r.path << " looks like a ccache wrapper" <<
              info << "ccache cannot be used as a " << xl << " compiler" <<
              info << "use config." << xm << " to override";
        }
      }

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

    static compiler_version
    msvc_compiler_version (string v)
    {
      compiler_version r;

      // Split the version into components.
      //
      size_t b (0), e (b);
      auto next = [&v, &b, &e] (const char* m) -> uint64_t
      {
        try
        {
          if (next_word (v, b, e, '.'))
            return stoull (string (v, b, e - b));
        }
        catch (const invalid_argument&) {}
        catch (const out_of_range&) {}

        fail << "unable to extract MSVC " << m << " version from '"
             << v << "'" << endf;
      };

      r.major = next ("major");
      r.minor = next ("minor");
      r.patch = next ("patch");

      if (next_word (v, b, e, '.'))
        r.build.assign (v, b, e - b);

      r.string = move (v);

      return r;
    }

    static string
    msvc_runtime_version (const compiler_version& v)
    {
      // Mapping of compiler versions to runtime versions:
      //
      // Note that VC 15 has runtime version 14.1 but the DLLs are still
      // called *140.dll (they are said to be backwards-compatible).
      //
      // And VC 16 seems to have the runtime version 14.1 (and not 14.2, as
      // one might expect; DLLs are still *140.dll but there are now _1 and _2
      // variants for, say, msvcp140.dll). We will, however, call it 14.2
      // (which is the version of the "toolset") in our target triplet. And we
      // will call VC 17 14.3 (which is also the version of the "toolset").
      //
      // year   ver   cl     crt/dll   toolset
      //
      // 2022   17.X  19.3X  14.?/140  14.3X
      // 2019   16.X  19.2X  14.2/140  14.2X
      // 2017   15.9  19.16  14.1/140  14.16
      // 2017   15.8  19.15  14.1/140
      // 2017   15.7  19.14  14.1/140
      // 2017   15.6  19.13  14.1/140
      // 2017   15.5  19.12  14.1/140
      // 2017   15.3  19.11  14.1/140
      // 2017   15    19.10  14.1/140
      // 2015   14    19.00  14.0/140
      // 2013   12    18.00  12.0/120
      // 2012   11    17.00  11.0/110
      // 2010   10    16.00  10.0/100
      // 2008    9    15.00   9.0/90
      // 2005    8    14.00   8.0/80
      // 2003  7.1    13.10   7.1/71
      //
      // _MSC_VER is the numeric cl version, e.g., 1921 for 19.21.
      //
      /**/ if (v.major == 19 && v.minor >= 30) return "14.3";
      else if (v.major == 19 && v.minor >= 20) return "14.2";
      else if (v.major == 19 && v.minor >= 10) return "14.1";
      else if (v.major == 19 && v.minor ==  0) return "14.0";
      else if (v.major == 18 && v.minor ==  0) return "12.0";
      else if (v.major == 17 && v.minor ==  0) return "11.0";
      else if (v.major == 16 && v.minor ==  0) return "10.0";
      else if (v.major == 15 && v.minor ==  0) return "9.0";
      else if (v.major == 14 && v.minor ==  0) return "8.0";
      else if (v.major == 13 && v.minor == 10) return "7.1";

      fail << "unable to map MSVC compiler version '" << v.string
           << "' to runtime version" << endf;
    }

    void
    msvc_extract_header_search_dirs (const strings&, dir_paths&); // msvc.cxx

    void
    msvc_extract_library_search_dirs (const strings&, dir_paths&); // msvc.cxx

    // Return the MSVC system header search paths (i.e., what the Visual
    // Studio command prompt puts into INCLUDE) including any paths from the
    // compiler mode and their count.
    //
    // Note that currently we don't add any ATL/MFC paths (but could do that
    // probably first checking if they exist/empty).
    //
    static pair<dir_paths, size_t>
    msvc_hdr (const msvc_info& mi, const strings& mo)
    {
      dir_paths r;

      // Extract /I paths and similar from the compiler mode.
      //
      msvc_extract_header_search_dirs (mo, r);
      size_t rn (r.size ());

      // Note: the resulting directories are normalized by construction.
      //
      r.push_back (dir_path (mi.msvc_dir) /= "include");

      // This path structure only appeared in Platform SDK 10 (if anyone wants
      // to use anything older, they will just have to use the MSVC command
      // prompt).
      //
      if (!mi.psdk_ver.empty ())
      {
        dir_path d ((dir_path (mi.psdk_dir) /= "Include") /= mi.psdk_ver);

        r.push_back (dir_path (d) /= "ucrt"  );
        r.push_back (dir_path (d) /= "shared");
        r.push_back (dir_path (d) /= "um"    );
        r.push_back (dir_path (d) /= "winrt" );
      }

      return make_pair (move (r), rn);
    }

    // Return the MSVC system module search paths (i.e., what the Visual
    // Studio command prompt puts into IFCPATH) including any paths from the
    // compiler mode and their count.
    //
    static pair<dir_paths, size_t>
    msvc_mod (const msvc_info& mi, const strings&, const char* cpu)
    {
      //@@ TODO: mode.

      dir_paths r;

      r.push_back ((dir_path (mi.msvc_dir) /= "ifc") /= cpu);

      return make_pair (move (r), size_t (0));
    }

    // Return the MSVC system library search paths (i.e., what the Visual
    // Studio command prompt puts into LIB) including any paths from the
    // compiler mode and their count.
    //
    static pair<dir_paths, size_t>
    msvc_lib (const msvc_info& mi, const strings& mo, const char* cpu)
    {
      dir_paths r;

      // Extract /LIBPATH paths from the compiler mode.
      //
      msvc_extract_library_search_dirs (mo, r);
      size_t rn (r.size ());

      // Note: the resulting directories are normalized by construction.
      //
      r.push_back ((dir_path (mi.msvc_dir) /= "lib") /= cpu);

      // This path structure only appeared in Platform SDK 10 (if anyone wants
      // to use anything older, they will just have to use the MSVC command
      // prompt).
      //
      if (!mi.psdk_ver.empty ())
      {
        dir_path d ((dir_path (mi.psdk_dir) /= "Lib") /= mi.psdk_ver);

        r.push_back ((dir_path (d) /= "ucrt") /= cpu);
        r.push_back ((dir_path (d) /= "um"  ) /= cpu);
      }

      return make_pair (move (r), rn);
    }

    // Return the MSVC binutils search paths (i.e., what the Visual Studio
    // command prompt puts into PATH).
    //
    static string
    msvc_bin (const msvc_info& mi, const char* cpu)
    {
      string r;

      // Seeing that we only do 64-bit on Windows, let's always use 64-bit
      // MSVC tools (link.exe, etc). In case of the Platform SDK, it's unclear
      // what the CPU signifies (host, target, both).
      //
      r  = (((dir_path (mi.msvc_dir) /= "bin") /= "Hostx64") /= cpu).
        representation ();

      r += path::traits_type::path_separator;

      r += (((dir_path (mi.psdk_dir) /= "bin") /= mi.psdk_ver) /= cpu).
        representation ();

      return r;
    }

    const char*
    msvc_cpu (const string&); // msvc.cxx

    // Note that LIB, LINK, and _LINK_ are technically link.exe's variables
    // but we include them in case linking is done via the compiler without
    // loading bin.ld. BTW, the same applies to rc.exe INCLUDE.
    //
    // See also the note on environment and caching below if adding any new
    // variables.
    //
    static const char* msvc_env[] = {"INCLUDE", "IFCPATH", "CL", "_CL_",
                                     "LIB", "LINK", "_LINK_", nullptr};

    static compiler_info
    guess_msvc (context&,
                const char* xm,
                lang xl,
                const path& xc,
                const string* xv,
                const string* xt,
                const strings& x_mo,
                const strings*, const strings*,
                const strings*, const strings*,
                const strings*, const strings*,
                guess_result&& gr, sha256&)
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
      // "ARM64"
      //
      compiler_version ver;
      {
        auto df = make_diag_frame (
          [&xm](const diag_record& dr)
          {
            dr << info << "use config." << xm << ".version to override";
          });

        // Treat the custom version as just a tail of the signature.
        //
        const string& s (xv == nullptr ? gr.signature : *xv);

        // Some overrides for testing.
        //
        //string s;
        //s = "Microsoft (R) 32-bit C/C++ Optimizing Compiler Version 15.00.30729.01 for 80x86";
        //s = "Compilador de optimizacion de C/C++ de Microsoft (R) version 16.00.30319.01 para x64";
        //s = "Compilateur d'optimisation Microsoft (R) C/C++ version 19.16.27026.1 pour x64";

        // Scan the string as words and look for the version.
        //
        size_t b (0), e (0);
        while (next_word (s, b, e, ' ', ','))
        {
          // The third argument to find_first_not_of() is the length of the
          // first argument, not the length of the interval to check. So to
          // limit it to [b, e) we are also going to compare the result to the
          // end of the word position (first space). In fact, we can just
          // check if it is >= e.
          //
          if (s.find_first_not_of ("1234567890.", b, 11) >= e)
            break;
        }

        if (b == e)
          fail << "unable to extract MSVC version from '" << s << "'";

        ver = msvc_compiler_version (string (s, b, e - b));
      }


      // Figure out the target architecture.
      //
      string t, ot;

      if (xt == nullptr)
      {
        auto df = make_diag_frame (
          [&xm](const diag_record& dr)
          {
            dr << info << "use config." << xm << ".target to override";
          });

        const string& s (gr.signature);

        // Scan the string as words and look for the CPU.
        //
        string cpu;

        for (size_t b (0), e (0), n;
             (n = next_word (s, b, e, ' ', ',')) != 0; )
        {
          if (s.compare (b, n, "x64",   3) == 0 ||
              s.compare (b, n, "x86",   3) == 0 ||
              s.compare (b, n, "ARM64", 5) == 0 ||
              s.compare (b, n, "ARM",   3) == 0 ||
              s.compare (b, n, "80x86", 5) == 0)
          {
            cpu.assign (s, b, n);
            break;
          }
        }

        if (cpu.empty ())
          fail << "unable to extract MSVC target CPU from " << "'" << s << "'";

        // Now we need to map x86, x64, ARM, and ARM64 to the target
        // triplets. The problem is, there aren't any established ones so we
        // got to invent them ourselves. Based on the discussion in
        // <libbutl/target-triplet.hxx>, we need something in the
        // CPU-VENDOR-OS-ABI form.
        //
        // The CPU part is fairly straightforward with x86 mapped to 'i386'
        // (or maybe 'i686'), x64 to 'x86_64', ARM to 'arm' (it could also
        // include the version, e.g., 'amrv8'), and ARM64 to 'aarch64'.
        //
        // The (toolchain) VENDOR is also straightforward: 'microsoft'. Why
        // not omit it? Two reasons: firstly, there are other compilers with
        // the otherwise same target, for example Intel C/C++, and it could be
        // useful to distinguish between them. Secondly, by having all four
        // components we remove any parsing ambiguity.
        //
        // OS-ABI is where things are not as clear cut. The OS part shouldn't
        // probably be just 'windows' since we have Win32 and WinCE. And
        // WinRT. And Universal Windows Platform (UWP). So perhaps the
        // following values for OS: 'win32', 'wince', 'winrt', 'winup'.
        //
        // For 'win32' the ABI part could signal the Microsoft C/C++ runtime
        // by calling it 'msvc'. And seeing that the runtimes are incompatible
        // from version to version, we should probably add the 'X.Y' version
        // at the end (so we essentially mimic the DLL name, for example,
        // msvcr120.dll). Some suggested we also encode the runtime type
        // (those pesky /M* options) though I am not sure: the only
        // "redistributable" runtime is multi-threaded release DLL.
        //
        // The ABI part for the other OS values needs thinking. For 'winrt'
        // and 'winup' it probably makes sense to encode the WINAPI_FAMILY
        // macro value (perhaps also with the version). Some of its values:
        //
        // WINAPI_FAMILY_APP        Windows 10
        // WINAPI_FAMILY_PC_APP     Windows 8.1
        // WINAPI_FAMILY_PHONE_APP  Windows Phone 8.1
        //
        // For 'wince' we may also want to add the OS version, for example,
        // 'wince4.2'.
        //
        // Putting it all together, Visual Studio 2015 will then have the
        // following target triplets:
        //
        // x86    i386-microsoft-win32-msvc14.0
        // x64    x86_64-microsoft-win32-msvc14.0
        // ARM    arm-microsoft-winup-???
        // ARM64  aarch64-microsoft-win32-msvc14.0
        //
        if (cpu == "ARM")
          fail << "cl.exe ARM/WinRT/UWP target is not yet supported";
        else
        {
          if (cpu == "x64")
            t = "x86_64-microsoft-win32-msvc";
          else if (cpu == "x86" || cpu == "80x86")
            t = "i386-microsoft-win32-msvc";
          else if (cpu == "ARM64")
            t = "aarch64-microsoft-win32-msvc";
          else
            assert (false);

          t += msvc_runtime_version (ver);
        }

        ot = t;
      }
      else
        ot = t = *xt;

      target_triplet tt (t); // Shouldn't fail.

      // If we have the MSVC installation information, then this means we are
      // running out of the Visual Studio command prompt and will have to
      // supply PATH/INCLUDE/LIB/IFCPATH equivalents ourselves.
      //
      optional<pair<dir_paths, size_t>> lib_dirs;
      optional<pair<dir_paths, size_t>> hdr_dirs;
      optional<pair<dir_paths, size_t>> mod_dirs;
      string bpat;

      if (const msvc_info* mi = static_cast<msvc_info*> (gr.info.get ()))
      {
        const char* cpu (msvc_cpu (tt.cpu));

        lib_dirs = msvc_lib (*mi, x_mo, cpu);
        hdr_dirs = msvc_hdr (*mi, x_mo);
        mod_dirs = msvc_mod (*mi, x_mo, cpu);

        bpat = msvc_bin (*mi, cpu);
      }

      // Derive the toolchain pattern.
      //
      // If the compiler name is/starts with 'cl' (e.g., cl.exe, cl-14),
      // then replace it with '*' and use it as a pattern for lib, link,
      // etc.
      //
      string cpat (pattern (xc, "cl", nullptr, ".-"));

      if (bpat.empty ())
        bpat = cpat; // Binutils pattern is the same as toolchain.

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
        move (ver),
        nullopt,
        move (gr.signature),
        "",
        move (t),
        move (ot),
        move (cpat),
        move (bpat),
        move (rt),
        move (csl),
        move (xsl),
        move (lib_dirs),
        move (hdr_dirs),
        move (mod_dirs),
        msvc_env,
        nullptr};
    }

    // See "Environment Variables Affecting GCC".
    //
    // Note that we also check below that the following variables are not set
    // since they would interfere with what we are doing.
    //
    // DEPENDENCIES_OUTPUT
    // SUNPRO_DEPENDENCIES
    //
    // Note also that we include (some) linker's variables in case linking is
    // done via the compiler without loading bin.ld (to do this precisely we
    // would need to detect which linker is being used at which point we might
    // as well load bin.ld).
    //
    // See also the note on environment and caching below if adding any new
    // variables.
    //
    static const char* gcc_c_env[] = {
      "CPATH", "C_INCLUDE_PATH",
      "LIBRARY_PATH", "LD_RUN_PATH",
      "SOURCE_DATE_EPOCH", "GCC_EXEC_PREFIX", "COMPILER_PATH",
      nullptr};

    static const char* gcc_cxx_env[] = {
      "CPATH", "CPLUS_INCLUDE_PATH",
      "LIBRARY_PATH", "LD_RUN_PATH",
      "SOURCE_DATE_EPOCH", "GCC_EXEC_PREFIX", "COMPILER_PATH",
      nullptr};

    // Note that Clang recognizes a whole family of *_DEPLOYMENT_TARGET
    // variables (as does ld64).
    //
    static const char* macos_env[] = {
      "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", nullptr};

    static compiler_info
    guess_gcc (context& ctx,
               const char* xm,
               lang xl,
               const path& xc,
               const string* xv,
               const string* xt,
               const strings& x_mo,
               const strings* c_po, const strings* x_po,
               const strings* c_co, const strings* x_co,
               const strings*, const strings*,
               guess_result&& gr, sha256&)
    {
      tracer trace ("cc::guess_gcc");

      const process_path& xp (gr.path);

      // Extract the version. The signature line has the following format
      // though language words can be translated and even rearranged (see
      // examples above).
      //
      // "gcc version X[.Y[.Z]][...]"
      //
      compiler_version ver;
      {
        auto df = make_diag_frame (
          [&xm](const diag_record& dr)
          {
            dr << info << "use config." << xm << ".version to override";
          });

        // Treat the custom version as just a tail of the signature.
        //
        const string& s (xv == nullptr ? gr.signature : *xv);

        // Scan the string as words and look for one that looks like a
        // version.
        //
        size_t b (0), e (0);
        while (next_word (s, b, e))
        {
          // The third argument to find_first_not_of() is the length of the
          // first argument, not the length of the interval to check. So to
          // limit it to [b, e) we are also going to compare the result to the
          // end of the word position (first space). In fact, we can just
          // check if it is >= e.
          //
          size_t p (s.find_first_not_of ("1234567890.", b, 11));
          if (p >= e || (p > b && (s[p] == '-' || s[p] == '+')))
            break;
        }

        if (b == e)
          fail << "unable to extract GCC version from '" << s << "'";

        // Split the version into components by parsing it as semantic-like
        // version.
        //
        try
        {
          semantic_version v (string (s, b, e - b),
                              semantic_version::allow_omit_minor |
                              semantic_version::allow_build,
                              ".-+");
          ver.major = v.major;
          ver.minor = v.minor;
          ver.patch = v.patch;
          ver.build = move (v.build);
        }
        catch (const invalid_argument& e)
        {
          fail << "unable to extract GCC version from '" << s << "': " << e;
        }

        ver.string.assign (s, b, string::npos);
      }

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
      string t, ot;

      if (xt == nullptr)
      {
        cstrings args {xp.recall_string ()};
        if (c_co != nullptr) append_options (args, *c_co);
        if (x_co != nullptr) append_options (args, *x_co);
        append_options (args, x_mo);
        args.push_back ("-print-multiarch"); // Note: position relied upon.
        args.push_back (nullptr);

        // The output of both -print-multiarch and -dumpmachine is a single
        // line containing just the target triplet. We don't expect any
        // localization so no need for LC_ALL.
        //
        auto f = [] (string& l, bool) {return move (l);};

        t = run<string> (ctx, 3, xp, args, f, false);

        if (t.empty ())
        {
          l5 ([&]{trace << xc << " doesn's support -print-multiarch, "
                        << "falling back to -dumpmachine";});

          args[args.size () - 2] = "-dumpmachine";
          t = run<string> (ctx, 3, xp, args, f, false);
        }

        if (t.empty ())
          fail << "unable to extract target architecture from " << xc
               << " using -print-multiarch or -dumpmachine output" <<
            info << "use config." << xm << ".target to override";

        ot = t;
      }
      else
        ot = t = *xt;

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
      string csl (
        tt.system == "mingw32"
        ? "msvc"
        : stdlib (xl, xp, x_mo, c_po, x_po, c_co, x_co, c_stdlib_src));
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

          xsl = stdlib (xl, xp, x_mo, c_po, x_po, c_co, x_co, src);
          break;
        }
      }

      // Environment.
      //
      if (getenv ("DEPENDENCIES_OUTPUT"))
        fail << "GCC DEPENDENCIES_OUTPUT environment variable is set";

      if (getenv ("SUNPRO_DEPENDENCIES"))
        fail << "GCC SUNPRO_DEPENDENCIES environment variable is set";

      const char* const* c_env (nullptr);
      switch (xl)
      {
      case lang::c:   c_env = gcc_c_env;   break;
      case lang::cxx: c_env = gcc_cxx_env; break;
      }

      const char* const* p_env (tt.system == "darwin" ? macos_env : nullptr);

      return compiler_info {
        move (gr.path),
        move (gr.id),
        compiler_class::gcc,
        move (ver),
        nullopt,
        move (gr.signature),
        move (gr.checksum), // Calculated on whole -v output.
        move (t),
        move (ot),
        move (pat),
        "",
        move (rt),
        move (csl),
        move (xsl),
        nullopt,
        nullopt,
        nullopt,
        c_env,
        p_env};
    }

    struct clang_msvc_info: msvc_info
    {
      string   triple;        // cc1 -triple value
      string   msvc_ver;      // Compiler version from triple.
      string   msvc_comp_ver; // cc1 -fms-compatibility-version value
    };

    static clang_msvc_info
    guess_clang_msvc (lang xl,
                      const process_path& xp,
                      const strings& x_mo,
                      const strings* c_co, const strings* x_co,
                      bool cl)
    {
      tracer trace ("cc::guess_clang_msvc");

      cstrings args {xp.recall_string ()};
      if (c_co != nullptr) append_options (args, *c_co);
      if (x_co != nullptr) append_options (args, *x_co);
      append_options (args, x_mo);

      if (cl)
      {
        switch (xl)
        {
        case lang::c:   args.push_back ("/TC"); break;
        case lang::cxx: args.push_back ("/TP"); break;
        }
      }
      else
      {
        args.push_back ("-x");
        switch (xl)
        {
        case lang::c:   args.push_back ("c");   break;
        case lang::cxx: args.push_back ("c++"); break;
        }
      }

      args.push_back ("-v");
      args.push_back ("-E");
      args.push_back ("-");  // Read stdin.
      args.push_back (nullptr);

      // The diagnostics we are interested in goes to stderr but we also get a
      // few lines of the preprocessed boilerplate at the end.
      //
      process pr (run_start (3     /* verbosity */,
                             xp,
                             args,
                             -2      /* stdin  (to /dev/null) */,
                             -1      /* stdout                */,
                             1       /* stderr (to stdout)    */));

      clang_msvc_info r;

      string l;
      try
      {
        // The overall structure of the output is as follows (with some
        // fragments that we are not interested in replaced with `...`):
        //
        // clang version 9.0.0 (tags/RELEASE_900/final)
        // ...
        // ...
        // InstalledDir: C:\Program Files\LLVM\bin
        //  "C:\\Program Files\\LLVM\\bin\\clang++.exe" -cc1 -triple x86_64-pc-windows-msvc19.23.28105 -fms-compatibility-version=19.23.28105 ..."
        // clang -cc1 version 9.0.0 based upon LLVM 9.0.0 default target x86_64-pc-windows-msvc
        // #include "..." search starts here:
        // #include <...> search starts here:
        //  C:\Program Files\LLVM\lib\clang\9.0.0\include
        //  C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.23.28105\include
        //  C:\Program Files (x86)\Windows Kits\10\Include\10.0.18362.0\ucrt
        //  ...
        // End of search list.
        // ...
        // ...
        //
        // Notice also that the version in the target triple and in the
        // ...VC\Tools\MSVC\ subdirectory are not exactly the same (and "how
        // the same" the are guaranteed to be is anyone's guess).
        //
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        for (bool in_include (false); !eof (getline (is, l)); )
        {
          l6 ([&]{trace << "examining line '" << l << "'";});

          if (r.triple.empty ())
          {
            size_t b, e;
            if ((b = l.find ("-triple "))  != string::npos &&
                (e = l.find (' ', b += 8)) != string::npos)
            {
              r.triple.assign (l, b, e - b);

              if ((b = r.triple.find ("-msvc")) == string::npos)
                fail << "no MSVC version in Clang target " << r.triple;

              r.msvc_ver.assign (r.triple, b += 5, string::npos);

              if ((b = l.find ("-fms-compatibility-version=")) != string::npos &&
                  (e = l.find (' ', b += 27)) != string::npos)
              {
                r.msvc_comp_ver.assign (l, b, e - b);
              }
              else
                r.msvc_comp_ver = r.msvc_ver;

              l5 ([&]{trace << "MSVC target " << r.triple
                            << ", version " << r.msvc_ver
                            << ", compatibility version " << r.msvc_comp_ver;});
            }

            continue;
          }

          // Note: similar logic to gcc_header_search_paths().
          //
          if (!in_include)
            in_include = l.find ("#include <...>") != string::npos;
          else
          {
            if (l[0] != ' ') // End of header search paths.
              break;

            try
            {
              dir_path d (move (trim (l)));

              l6 ([&]{trace << "examining directory " << d;});

              auto b (d.begin ()), e (d.end ());

              if (r.msvc_dir.empty ())
              {
                // Look for the "Tools\MSVC\<ver>\include" component sequence.
                //
                auto i (find_if (b, e,
                                 [] (const string& n)
                                 {
                                   return icasecmp (n, "Tools") == 0;
                                 }));

                if (i != e                                   &&
                    (++i != e && icasecmp (*i, "MSVC") == 0) &&
                    (++i != e                              ) &&
                    (++i != e && icasecmp (*i, "include") == 0))
                {
                  r.msvc_dir = dir_path (b, i);

                  l5 ([&]{trace << "MSVC directory " << r.msvc_dir;});
                }
              }

              if (r.psdk_dir.empty ())
              {
                // Look for the "Windows Kits\<ver>\Include" component
                // sequence.
                //
                // Note that the path structure differs between 10 and pre-10
                // versions:
                //
                // ...\Windows Kits\10\Include\10.0.18362.0\...
                // ...\Windows Kits\8.1\Include\...
                //
                auto i (find_if (b, e,
                                 [] (const string& n)
                                 {
                                   return icasecmp (n, "Windows Kits") == 0;
                                 })), j (i);

                if (i != e                                   &&
                    (++i != e                              ) &&
                    (++i != e && icasecmp (*i, "Include") == 0))
                {
                  r.psdk_dir = dir_path (b, i);

                  if (*++j == "10" && ++i != e)
                    r.psdk_ver = *i;

                  l5 ([&]{trace << "Platform SDK directory " << r.psdk_dir
                                << ", version '" << r.psdk_ver << "'";});
                }
              }
            }
            catch (const invalid_path&)
            {
              // Skip this path.
            }

            if (!r.msvc_dir.empty () && !r.psdk_dir.empty ())
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

      if (!run_finish_code (args.data (), pr, l, 2 /* verbosity */))
        fail << "unable to extract MSVC information from " << xp;

      if (const char* w = (
            r.triple.empty ()        ? "MSVC target" :
            r.msvc_ver.empty ()      ? "MSVC version" :
            r.msvc_comp_ver.empty () ? "MSVC compatibility version" :
            r.msvc_dir.empty ()      ? "MSVC directory" :
            r.psdk_dir.empty ()      ? "Platform SDK directory":
            nullptr))
        fail << "unable to extract " << w << " from " << xp;

      return r;
    }

    // These are derived from gcc_* plus the sparse documentation (clang(1))
    // and source code.
    //
    // Note that for now for Clang targeting MSVC we use msvc_env but should
    // probably use a combined list.
    //
    // See also the note on environment and caching below if adding any new
    // variables.
    //
    static const char* clang_c_env[] = {
      "CPATH", "C_INCLUDE_PATH", "CCC_OVERRIDE_OPTIONS",
      "LIBRARY_PATH", "LD_RUN_PATH",
      "COMPILER_PATH",
      nullptr};

    static const char* clang_cxx_env[] = {
      "CPATH", "CPLUS_INCLUDE_PATH", "CCC_OVERRIDE_OPTIONS",
      "LIBRARY_PATH", "LD_RUN_PATH",
      "COMPILER_PATH",
      nullptr};

    static compiler_info
    guess_clang (context& ctx,
                 const char* xm,
                 lang xl,
                 const path& xc,
                 const string* xv,
                 const string* xt,
                 const strings& x_mo,
                 const strings* c_po, const strings* x_po,
                 const strings* c_co, const strings* x_co,
                 const strings* c_lo, const strings* x_lo,
                 guess_result&& gr, sha256& cs)
    {
      // This function handles vanilla Clang, including its clang-cl variant,
      // as well as Apple and Emscripten variants.
      //
      // The clang-cl variant appears to be a very thin wrapper over the
      // standard clang/clang++ drivers. In addition to the cl options, it
      // mostly accepts standard Clang options with a few exceptions (notably
      // -x). It also has /clang:<arg> to pass things down to the driver
      // (which for some reason doesn't work for -x).
      //
      bool cl (gr.id.type == compiler_type::msvc);
      bool apple (gr.id.variant == "apple");
      bool emscr (gr.id.variant == "emscripten");

      const process_path& xp (gr.path);

      // Extract the version. Here we will try to handle both vanilla and
      // Apple Clang since the signature lines are fairly similar. They have
      // the following format though language words can probably be translated
      // and even rearranged (see examples above).
      //
      // "[... ]clang version A.B.C[( |-)...]"
      // "Apple (clang|LLVM) version A.B[.C] ..."
      //
      // We will also reuse this code to parse the Emscripten version which
      // is quite similar:
      //
      // emcc (...) 2.0.8
      //
      // Pre-releases of the vanilla Clang append `rc` or `git` to the
      // version, unfortunately without a separator. So we will handle these
      // ad hoc. For example:
      //
      // FreeBSD clang version 18.1.0rc (https://github.com/llvm/llvm-project.git llvmorg-18-init-18361-g22683463740e)
      //
      auto extract_version = [] (const string& s, bool patch, const char* what)
        -> compiler_version
      {
        compiler_version ver;

        size_t b (0), e (0);
        while (next_word (s, b, e, ' ', '-'))
        {
          // The third argument to find_first_not_of() is the length of the
          // first argument, not the length of the interval to check. So to
          // limit it to [b, e) we are also going to compare the result to the
          // end of the word position (first space). In fact, we can just
          // check if it is >= e.
          //
          size_t p (s.find_first_not_of ("1234567890.", b, 11));
          if (p >= e)
            break;

          // Handle the unseparated `rc` and `git` suffixes.
          //
          if (p != string::npos)
          {
            if (p + 2 == e && (e - b) > 2 &&
                s[p] == 'r' && s[p + 1] == 'c')
            {
              e -= 2;
              break;
            }

            if (p + 3 == e && (e - b) > 3 &&
                s[p] == 'g' && s[p + 1] == 'i' && s[p + 2] == 't')
            {
              e -= 3;
              break;
            }
          }
        }

        if (b == e)
          fail << "unable to extract " << what << " version from '" << s << "'"
               << endf;

        ver.string.assign (s, b, string::npos);

        // Split the version into components.
        //
        size_t vb (b), ve (b);
        auto next = [&s, what,
                     b, e,
                     &vb, &ve] (const char* m, bool opt) -> uint64_t
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

          fail << "unable to extract " << what << ' ' << m << " version from '"
               << string (s, b, e - b) << "'" << endf;
        };

        ver.major = next ("major", false);
        ver.minor = next ("minor", false);
        ver.patch = next ("patch", patch);

        if (e != s.size ())
        {
          // Skip the separator (it could also be unseparated `rc` or `git`).
          //
          if (s[e] == ' ' || s[e] == '-')
            e++;

          ver.build.assign (s, e, string::npos);
        }

        return ver;
      };

      compiler_version ver;
      {
        auto df = make_diag_frame (
          [&xm](const diag_record& dr)
          {
            dr << info << "use config." << xm << ".version to override";
          });

        // Treat the custom version as just a tail of the signature.
        //
        // @@ TODO: should we have type_version here (and suggest that
        //          in diag_frame above?
        //
        const string& s (xv != nullptr
                         ? *xv
                         : emscr ? gr.type_signature : gr.signature);

        // Some overrides for testing.
        //
        //string s (xv != nullptr ? *xv : "");
        //
        //s = "clang version 3.7.0 (tags/RELEASE_370/final)";
        //s = "FreeBSD clang version 18.1.0rc (https://github.com/llvm/llvm-project.git llvmorg-18-init-18361-g22683463740e)";
        //
        //gr.id.variant = "apple";
        //s = "Apple LLVM version 7.3.0 (clang-703.0.16.1)";
        //s = "Apple clang version 3.1 (tags/Apple/clang-318.0.58) (based on LLVM 3.1svn)";

        // Scan the string as words and look for one that looks like a
        // version. Use '-' as a second delimiter to handle versions like
        // "3.6.0-2ubuntu1~trusty1".
        //
        ver = extract_version (s, apple, "Clang");
      }

      optional<compiler_version> var_ver;
      if (apple)
      {
        // Map Apple to vanilla Clang version, preserving the original as the
        // variant version.
        //
        var_ver = move (ver);

        // Apple no longer discloses the mapping so it's a guesswork and we
        // better be conservative. For details see:
        //
        // https://gist.github.com/yamaya/2924292
        //
        // Specifically, we now look in the libc++'s __config file for the
        // _LIBCPP_VERSION and use the previous version as a conservative
        // estimate (NOTE: that there could be multiple __config files with
        // potentially different versions so compile with -v to see which one
        // gets picked up).
        //
        // Also, lately, we started seeing _LIBCPP_VERSION values like 15.0.6
        // or 16.0.2 which would suggest the base is 15.0.5 or 16.0.1. But
        // that assumption did not check out with the actual usage. For
        // example, vanilla Clang 16 should no longer require -fmodules-ts but
        // the Apple's version (that is presumably based on it) still does. So
        // the theory here is that Apple upgrades to newer libc++ while
        // keeping the old compiler. Which means we must be more conservative
        // and assume something like 15.0.6 is still 14-based. But then you
        // get -Wunqualified-std-cast-call in 14, which was supposedly only
        // introduced in Clang 15. So maybe not.
        //
        // Note that this is Apple Clang version and not XCode version.
        //
        // 4.2      -> 3.2svn
        // 5.0      -> 3.3svn
        // 5.1      -> 3.4svn
        // 6.0      -> 3.5svn
        // 6.1.0    -> 3.6svn
        // 7.0.0    -> 3.7
        // 7.3.0    -> 3.8
        // 8.0.0    -> 3.9
        // 8.1.0    -> ?
        // 9.0.0    -> 4.0
        // 9.1.0    -> 5.0
        // 10.0.0   -> 6.0
        // 11.0.0   -> 7.0
        // 11.0.3   -> 8.0  (yes, seriously!)
        // 12.0.0   -> 9.0
        // 12.0.5   -> 10.0 (yes, seriously!)
        // 13.0.0   -> 11.0
        // 13.1.6   -> 12.0
        // 14.0.0   -> 12.0 (_LIBCPP_VERSION=130000)
        // 14.0.3   -> 15.0 (_LIBCPP_VERSION=150006)
        // 15.0.0.0 -> 16.0 (_LIBCPP_VERSION=160002)
        // 15.0.0.1 -> 16.0 (_LIBCPP_VERSION=160006)
        // 15.0.0.3 -> 16.0 (_LIBCPP_VERSION=170006)
        //
        uint64_t mj (var_ver->major);
        uint64_t mi (var_ver->minor);
        uint64_t pa (var_ver->patch);

        if      (mj >= 15)                          {mj = 16; mi = 0; pa = 0;}
        else if (mj == 14 && (mi > 0 || pa >= 3))   {mj = 15; mi = 0; pa = 0;}
        else if (mj == 14 || (mj == 13 && mi >= 1)) {mj = 12; mi = 0; pa = 0;}
        else if (mj == 13)                          {mj = 11; mi = 0; pa = 0;}
        else if (mj == 12 && (mi > 0 || pa >= 5))   {mj = 10; mi = 0; pa = 0;}
        else if (mj == 12)                          {mj = 9;  mi = 0; pa = 0;}
        else if (mj == 11 && (mi > 0 || pa >= 3))   {mj = 8;  mi = 0; pa = 0;}
        else if (mj == 11)                          {mj = 7;  mi = 0; pa = 0;}
        else if (mj == 10)                          {mj = 6;  mi = 0; pa = 0;}
        else if (mj == 9 && mi >= 1)                {mj = 5;  mi = 0; pa = 0;}
        else if (mj == 9)                           {mj = 4;  mi = 0; pa = 0;}
        else if (mj == 8)                           {mj = 3;  mi = 9; pa = 0;}
        else if (mj == 7 && mi >= 3)                {mj = 3;  mi = 8; pa = 0;}
        else if (mj == 7)                           {mj = 3;  mi = 7; pa = 0;}
        else if (mj == 6 && mi >= 1)                {mj = 3;  mi = 5; pa = 0;}
        else if (mj == 6)                           {mj = 3;  mi = 4; pa = 0;}
        else if (mj == 5 && mi >= 1)                {mj = 3;  mi = 3; pa = 0;}
        else if (mj == 5)                           {mj = 3;  mi = 2; pa = 0;}
        else if (mj == 4 && mi >= 2)                {mj = 3;  mi = 1; pa = 0;}
        else                                        {mj = 3;  mi = 0; pa = 0;}

        ver = compiler_version {
          to_string (mj) + '.' + to_string (mi) + '.' + to_string (pa),
          mj,
          mi,
          pa,
          ""};
      }
      else if (emscr)
      {
        // Extract Emscripten version.
        //
        auto df = make_diag_frame (
          [&xm](const diag_record& dr)
          {
            dr << info << "use config." << xm << ".version to override";
          });

        var_ver = extract_version (gr.signature, false, "Emscripten");

        // The official Emscripten distributions routinely use unreleased
        // Clang snapshots which nevertheless have the next release version
        // (which means it's actually somewhere between the previous release
        // and the next release). On the other hand, distributions like Debian
        // package it to use their Clang package which normally has the
        // accurate version. So here we will try to detect the former and
        // similar to the Apple case we will conservatively adjust it to the
        // previous release.
        //
        if (gr.type_signature.find ("googlesource") != string::npos)
        {
          if      (ver.patch != 0) ver.patch--;
          else if (ver.minor != 0) ver.minor--;
          else                     ver.major--;
        }
      }

      // Figure out the target architecture.
      //
      // Unlike gcc, clang doesn't have -print-multiarch. Its -dumpmachine,
      // however, respects the compile options (e.g., -m32).
      //
      string t, ot;

      if (xt == nullptr)
      {
        cstrings args {xp.recall_string ()};
        if (c_co != nullptr) append_options (args, *c_co);
        if (x_co != nullptr) append_options (args, *x_co);
        append_options (args, x_mo);
        args.push_back (cl ? "/clang:-dumpmachine" : "-dumpmachine");
        args.push_back (nullptr);

        // The output of -dumpmachine is a single line containing just the
        // target triplet. Again, we don't expect any localization so no need
        // for LC_ALL.
        //
        auto f = [] (string& l, bool) {return move (l);};
        t = run<string> (ctx, 3, xp, args, f, false);

        if (t.empty ())
          fail << "unable to extract target architecture from " << xc
               << " using -dumpmachine output" <<
            info << "use config." << xm << ".target to override";

        ot = t;
      }
      else
        ot = t = *xt;

      // Parse the target into triplet (for further tests) ignoring any
      // failures.
      //
      target_triplet tt;
      try {tt = target_triplet (t);} catch (const invalid_argument&) {}

      // For Clang on Windows targeting MSVC we remap the target to match
      // MSVC's.
      //
      optional<pair<dir_paths, size_t>> lib_dirs;
      string bpat;

      if (tt.system == "windows-msvc")
      {
        // Note that currently there is no straightforward way to determine
        // the VC version Clang is using. See:
        //
        // http://lists.llvm.org/pipermail/cfe-dev/2017-December/056240.html
        //
        // So we have to sniff this information out from Clang's -v output
        // (plus a couple of other useful bits like the VC installation
        // directory and Platform SDK).
        //
        clang_msvc_info mi (guess_clang_msvc (xl, xp, x_mo, c_co, x_co, cl));

        // Keep the CPU and replace the rest.
        //
        tt.vendor = "microsoft";
        tt.system = "win32-msvc";
        tt.version = msvc_runtime_version (msvc_compiler_version (mi.msvc_ver));
        t = tt.representation ();

        // Add the MSVC information to the signature and checksum.
        //
        if (cs.empty ())
          cs.append (gr.signature);

        cs.append (mi.msvc_ver);
        cs.append (mi.msvc_dir.string ());
        cs.append (mi.psdk_ver);
        cs.append (mi.psdk_dir.string ());

        gr.signature += " MSVC version ";
        gr.signature += mi.msvc_ver;

        const char* cpu (msvc_cpu (tt.cpu));

        // Come up with the system library search paths. Ideally we would want
        // to extract this from Clang and -print-search-dirs would have been
        // the natural way for Clang to report it. But no luck.
        //
        lib_dirs = msvc_lib (mi, x_mo, cpu);

        // Binutils search paths.
        //
        // We shouldn't need them if we are running from the command prompt
        // and omitting them in this case would also result in tidier command
        // lines. However, reliably detecting this and making sure the result
        // matches Clang's is complex. So let's keep it simple for now.
        //
        bpat = msvc_bin (mi, cpu);

        // If this is clang-cl, then use the MSVC compatibility version as its
        // primary version.
        //
        if (cl)
        {
          var_ver = move (ver);
          ver = msvc_compiler_version (mi.msvc_comp_ver);
        }
      }

      // Derive the compiler toolchain pattern.
      //
      string cpat;

      if (cl)
        ;
      else if (emscr)
      {
        cpat = pattern (xc, xl == lang::c ? "emcc" : "em++");

        // Emscripten provides the emar/emranlib wrappers (over llvm-*).
        //
        bpat = pattern (xc, xl == lang::c ? "cc" : "++", "m");
      }
      else
      {
        // Try clang/clang++, the gcc/g++ alias, as well as cc/c++.
        //
        cpat = pattern (xc, xl == lang::c ? "clang" : "clang++");

        if (cpat.empty ())
          cpat = pattern (xc, xl == lang::c ? "gcc" : "g++");

        if (cpat.empty ())
          cpat = pattern (xc, xl == lang::c ? "cc" : "c++");
      }

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
        if ((o = find_rtlib (&x_mo)) != nullptr ||
            (o = find_rtlib (x_lo))  != nullptr ||
            (o = find_rtlib (c_lo))  != nullptr)
        {
          rt = string (*o, 8);
        }
        else if (tt.system == "win32-msvc")  rt = "msvc";
        else if (tt.system == "linux-gnu" ||
                 tt.system == "freebsd"   ||
                 tt.system == "netbsd")      rt = "libgcc";
        else /* Mac OS, etc. */              rt = "compiler-rt";
      }

      string csl (
        tt.system == "win32-msvc" || tt.system == "mingw32"
        ? "msvc"
        : stdlib (xl, xp, x_mo, c_po, x_po, c_co, x_co, c_stdlib_src));

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
            : stdlib (xl, xp, x_mo, c_po, x_po, c_co, x_co, src);
          break;
        }
      }

      // Environment.
      //
      // Note that "Emscripten Compiler Frontend (emcc)" has a long list of
      // environment variables with little explanation. So someone will need
      // to figure out what's important (some of them are clearly for
      // debugging of emcc itself).
      //
      const char* const* c_env (nullptr);
      const char* const* p_env (nullptr);
      if (tt.system == "win32-msvc")
        c_env = msvc_env;
      else
      {
        switch (xl)
        {
        case lang::c:   c_env = clang_c_env;   break;
        case lang::cxx: c_env = clang_cxx_env; break;
        }

        if (tt.system == "darwin")
          p_env = macos_env;
      }

      return compiler_info {
        move (gr.path),
        move (gr.id),
        cl ? compiler_class::msvc : compiler_class::gcc,
        move (ver),
        move (var_ver),
        move (gr.signature),
        move (gr.checksum), // Calculated on whole -v output.
        move (t),
        move (ot),
        move (cpat),
        move (bpat),
        move (rt),
        move (csl),
        move (xsl),
        move (lib_dirs),
        nullopt,
        nullopt,
        c_env,
        p_env};
    }

    static compiler_info
    guess_icc (context& ctx,
               const char* xm,
               lang xl,
               const path& xc,
               const string* xv,
               const string* xt,
               const strings& x_mo,
               const strings* c_po, const strings* x_po,
               const strings* c_co, const strings* x_co,
               const strings*, const strings*,
               guess_result&& gr, sha256&)
    {
      //@@ TODO: this should be reviewed/revised if/when we get access
      //         to more recent ICC versions.

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
      // and even rearranged. Thus pass LC_ALL=C.
      //
      process_env env (xp);

#ifndef _WIN32
      const char* evars[] = {"LC_ALL=C", nullptr};
      env.vars = evars;
#endif

      auto f = [] (string& l, bool)
      {
        return l.compare (0, 5, "Intel") == 0 && (l[5] == '(' || l[5] == ' ')
        ? move (l)
        : string ();
      };

      if (xv == nullptr)
      {
        string& s (gr.signature);
        s.clear ();

        // The -V output is sent to STDERR.
        //
        // @@ TODO: running without the mode options.
        //
        s = run<string> (ctx, 3, env, "-V", f, false);

        if (s.empty ())
          fail << "unable to extract signature from " << xc << " -V output";

        if (s.find (xl == lang::c ? " C " : " C++ ") == string::npos)
          fail << xc << " does not appear to be the Intel " << xl
               << " compiler" <<
            info << "extracted signature: '" << s << "'";
      }

      // Scan the string as words and look for the version. It consist of only
      // digits and periods and contains at least one period.
      //
      compiler_version ver;
      {
        auto df = make_diag_frame (
          [&xm](const diag_record& dr)
          {
            dr << info << "use config." << xm << ".version to override";
          });

        // Treat the custom version as just a tail of the signature.
        //
        const string& s (xv == nullptr ? gr.signature : *xv);

        // Some overrides for testing.
        //
        //s = "Intel(R) C++ Compiler for 32-bit applications, Version 9.1 Build 20070215Z Package ID: l_cc_c_9.1.047";
        //s = "Intel(R) C++ Compiler for applications running on Intel(R) 64, Version 10.1 Build 20071116";
        //s = "Intel(R) C++ Compiler for applications running on IA-32, Version 10.1 Build 20071116 Package ID: l_cc_p_10.1.010";
        //s = "Intel C++ Intel 64 Compiler Professional for applications running on Intel 64, Version 11.0 Build 20081105 Package ID: l_cproc_p_11.0.074";
        //s = "Intel(R) C++ Intel(R) 64 Compiler Professional for applications running on Intel(R) 64, Version 11.1 Build 20091130 Package ID: l_cproc_p_11.1.064";
        //s = "Intel C++ Intel 64 Compiler XE for applications running on Intel 64, Version 12.0.4.191 Build 20110427";

        size_t b (0), e (0);
        while (next_word (s, b, e, ' ', ',') != 0)
        {
          // The third argument to find_first_not_of() is the length of the
          // first argument, not the length of the interval to check. So to
          // limit it to [b, e) we are also going to compare the result to the
          // end of the word position (first space). In fact, we can just
          // check if it is >= e. Similar logic for find_first_of() except
          // that we add space to the list of character to make sure we don't
          // go too far.
          //
          if (s.find_first_not_of ("1234567890.", b, 11) >= e &&
              s.find_first_of (". ", b, 2) < e)
            break;
        }

        if (b == e)
          fail << "unable to extract ICC version from '" << s << "'";

        ver.string.assign (s, b, string::npos);

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

          fail << "unable to extract ICC " << m << " version from '"
               << string (s, b, e - b) << "'" << endf;
        };

        ver.major = next ("major", false);
        ver.minor = next ("minor", false);
        ver.patch = next ("patch", true);

        if (vb != ve && next_word (s, e, vb, ve, '.'))
          ver.build.assign (s, vb, ve - vb);

        if (e != s.size ())
        {
          if (!ver.build.empty ())
            ver.build += ' ';

          ver.build.append (s, e + 1, string::npos);
        }
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
      // @@ TODO: why can't we combine it with the previous -V run?
      //
      string t, ot;

      if (xt == nullptr)
      {
        auto df = make_diag_frame (
          [&xm](const diag_record& dr)
          {
            dr << info << "use config." << xm << ".target to override";
          });

        cstrings args {xp.recall_string ()};
        if (c_co != nullptr) append_options (args, *c_co);
        if (x_co != nullptr) append_options (args, *x_co);
        append_options (args, x_mo);
        args.push_back ("-V");
        args.push_back (nullptr);

        // The -V output is sent to STDERR.
        //
        t = run<string> (ctx, 3, env, args, f, false);

        if (t.empty ())
          fail << "unable to extract target architecture from " << xc
               << " -V output";

        string arch;
        for (size_t b (0), e (0), n;
             (n = next_word (t, b, e, ' ', ',')) != 0; )
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
          fail << "unable to extract ICC target architecture from '"
               << t << "'";

        // So we have the CPU but we still need the rest of the triplet. While
        // icc currently doesn't support cross-compilation (at least on Linux)
        // and we could have just used the build triplet (i.e., the
        // architecture on which we are running), who knows what will happen
        // in the future. So instead we are going to use -dumpmachine and
        // substitute the CPU.
        //
        // Note: no localication expected so running without LC_ALL.
        //
        // @@ TODO: running without the mode options.
        //
        {
          auto f = [] (string& l, bool) {return move (l);};
          t = run<string> (ctx, 3, xp, "-dumpmachine", f);
        }

        if (t.empty ())
          fail << "unable to extract target architecture from " << xc
               << " using -dumpmachine output";

        // The first component in the triplet is always CPU.
        //
        size_t p (t.find ('-'));

        if (p == string::npos)
          fail << "unable to parse ICC target architecture '" << t << "'";

        t.swap (arch);
        t.append (arch, p, string::npos);

        ot = t;
      }
      else
        ot = t = *xt;

      // Parse the target into triplet (for further tests) ignoring any
      // failures.
      //
      target_triplet tt;
      try {tt = target_triplet (t);} catch (const invalid_argument&) {}

      // Derive the toolchain pattern.
      //
      string pat (pattern (xc, xl == lang::c ? "icc" : "icpc"));

      // Runtime and standard library.
      //
      // For now we assume that unless it is Windows, we are targeting
      // Linux/GCC.
      //
      string rt  (tt.system == "win32-msvc" ? "msvc" : "libgcc");
      string csl (
        tt.system == "win32-msvc"
        ? "msvc"
        : stdlib (xl, xp, x_mo, c_po, x_po, c_co, x_co, c_stdlib_src));
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
        move (ver),
        nullopt,
        move (gr.signature),
        "",
        move (t),
        move (ot),
        move (pat),
        "",
        move (rt),
        move (csl),
        move (xsl),
        nullopt,
        nullopt,
        nullopt,
        nullptr, /* TODO */
        nullptr};
    }

    // Compiler checks can be expensive (we often need to run the compiler
    // several times) so we cache the result.
    //
    static global_cache<compiler_info> cache;

    const compiler_info&
    guess (context& ctx,
           const char* xm,
           lang xl,
           const string& ec,
           const path& xc,
           const string* xis,
           const string* xv,
           const string* xt,
           const strings& x_mo,
           const strings* c_po, const strings* x_po,
           const strings* c_co, const strings* x_co,
           const strings* c_lo, const strings* x_lo)
    {
      // First check the cache.
      //
      // Note that in case of MSVC (and Clang targeting MSVC) sys_*_dirs can
      // be affected by the environment (INCLUDE, LIB, and IFCPATH) which is
      // project-specific. So we have to include those into the key. But we
      // don't know yet know whether it's those compilers/targets. So it seems
      // we have no better choice than to include the project environment if
      // overridden.
      //
      // @@ We currently include config.{cc,x}.[pc]options into the key which
      //    means any project-specific tweaks to these result in a different
      //    key. Perhaps we should assume that any options that can affect the
      //    result of what we are guessing (-m32, -stdlib=, etc) should be
      //    specified as part of the mode? While definitely feels correct,
      //    people will most likely specify these options else where as well.
      //
      string key;
      {
        sha256 cs;
        cs.append (static_cast<size_t> (xl));
        cs.append (xc.string ());
        if (!ec.empty ()) cs.append (ec);
        if (xis != nullptr) cs.append (*xis);
        append_options (cs, x_mo);
        if (c_po != nullptr) append_options (cs, *c_po);
        if (x_po != nullptr) append_options (cs, *x_po);
        if (c_co != nullptr) append_options (cs, *c_co);
        if (x_co != nullptr) append_options (cs, *x_co);
        if (c_lo != nullptr) append_options (cs, *c_lo);
        if (x_lo != nullptr) append_options (cs, *x_lo);
        key = cs.string ();

        if (const compiler_info* r = cache.find (key))
          return *r;
      }

      // Parse the user-specified compiler id (config.x.id).
      //
      optional<compiler_id> xi;
      if (xis != nullptr)
      {
        try
        {
          xi = compiler_id (*xis);
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid compiler id '" << *xis << "' "
               << "specified in variable config." << xm << ".id: " << e;
        }
      }

      pre_guess_result pre (pre_guess (xl, xc, xi));

      // If we could pre-guess the type based on the excutable name, then
      // try the test just for that compiler.
      //
      guess_result gr;
      sha256 cs;

      if (pre.type != invalid_compiler_type)
      {
        gr = guess (ctx, xm, xl, xc, x_mo, xi, pre, cs);

        if (gr.empty ())
        {
          warn << xc << " looks like " << pre << " but it is not" <<
            info << "use config." << xm << " to override";

          // Clear pre-guess.
          //
          pre.type = invalid_compiler_type;
          pre.variant = nullopt;
          pre.pos = string::npos;
        }
      }

      if (gr.empty ())
        gr = guess (ctx, xm, xl, xc, x_mo, xi, pre, cs);

      if (gr.empty ())
        fail << "unable to guess " << xl << " compiler type of " << xc <<
          info << "use config." << xm << ".id to specify explicitly";

      compiler_info (*gf) (
        context&,
        const char*, lang, const path&, const string*, const string*,
        const strings&,
        const strings*, const strings*,
        const strings*, const strings*,
        const strings*, const strings*,
        guess_result&&, sha256&) = nullptr;

      switch (gr.id.type)
      {
      case compiler_type::gcc:   gf = &guess_gcc;   break;
      case compiler_type::clang: gf = &guess_clang; break;
      case compiler_type::msvc:
        {
          gf = gr.id.variant == "clang" ? &guess_clang : &guess_msvc;
          break;
        }
      case compiler_type::icc: gf = &guess_icc;     break;
      }

      compiler_info r (gf (ctx,
                           xm, xl, xc, xv, xt,
                           x_mo, c_po, x_po, c_co, x_co, c_lo, x_lo,
                           move (gr), cs));

      // By default use the signature line(s) to generate the checksum.
      //
      if (cs.empty ())
      {
        cs.append (r.signature);

        if (!gr.type_signature.empty ())
          cs.append (gr.type_signature);
      }

      r.checksum = cs.string ();

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
      // BTW, for GCC we also get gcc-{ar,ranlib} (but not gcc-ld) which add
      // support for the LTO plugin though it seems more recent GNU binutils
      // (2.25) are able to load the plugin when needed automatically. So it
      // doesn't seem we should bother trying to support this on our end (one
      // way we could do it is by passing config.bin.{ar,ranlib} as hints).
      //
      // It's also normal for native (i.e., non-cross-compiler) builds of GCC
      // and Clang to not have binutils installed in the same directory and
      // instead relying on the system ones. In this case, if the compiler is
      // specified with the absolute path, the pattern will be the search
      // path.
      //
      if (r.bin_pattern.empty ())
      {
        if (pre.pos != 0 &&
            pre.pos != string::npos &&
            !path::traits_type::is_separator (xc.string ()[pre.pos - 1]))
        {
          r.bin_pattern.assign (xc.string (), 0, pre.pos);
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
      // search path.
      //
      if (r.bin_pattern.empty ())
      {
        const path& p (r.path.recall.empty () ? xc : r.path.recall);

        if (!p.simple ())
          r.bin_pattern = p.directory ().representation (); // Trailing slash.
      }

      return cache.insert (move (key), move (r));
    }

    strings
    guess_default (lang xl,
                   const string& cid,
                   const string& pat,
                   const strings& mode)
    {
      compiler_id id (cid);
      const char* s (nullptr);

      using type = compiler_type;

      switch (xl)
      {
      case lang::c:
        {
          switch (id.type)
          {
          case type::gcc:    s = "gcc";   break;
          case type::clang:
            {
              if (id.variant == "emscripten")
                s = "emcc";
              else
                s = "clang";
              break;
            }
          case type::icc:    s = "icc";   break;
          case type::msvc:
            {
              s = (id.variant == "clang" ? "clang-cl" : "cl");
              break;
            }
          }

          break;
        }
      case lang::cxx:
        {
          switch (id.type)
          {
          case type::gcc:    s = "g++";     break;
          case type::clang:
            {
              if (id.variant == "emscripten")
                s = "em++";
              else
                s = "clang++";
              break;
            }
          case type::icc:    s = "icpc";    break;
          case type::msvc:
            {
              s = (id.variant == "clang" ? "clang-cl" : "cl");
              break;
            }
          }

          break;
        }
      }

      strings r;
      r.reserve (mode.size () + 1);
      r.push_back (apply_pattern (s, pat));
      r.insert (r.end (), mode.begin (), mode.end ());

      return r;
    }

    // Table 23 [tab:headers.cpp].
    //
    // In the future we will probably have to maintain per-standard additions.
    //
    static const char* std_importable[] = {
      "<initializer_list>", // Note: keep first (present in freestanding).
      "<algorithm>",
      "<any>",
      "<array>",
      "<atomic>",
      "<barrier>",
      "<bit>",
      "<bitset>",
      "<charconv>",
      "<chrono>",
      "<codecvt>",
      "<compare>",
      "<complex>",
      "<concepts>",
      "<condition_variable>",
      "<coroutine>",
      "<deque>",
      "<exception>",
      "<execution>",
      "<filesystem>",
      "<format>",
      "<forward_list>",
      "<fstream>",
      "<functional>",
      "<future>",
      "<iomanip>",
      "<ios>",
      "<iosfwd>",
      "<iostream>",
      "<istream>",
      "<iterator>",
      "<latch>",
      "<limits>",
      "<list>",
      "<locale>",
      "<map>",
      "<memory>",
      "<memory_resource>",
      "<mutex>",
      "<new>",
      "<numbers>",
      "<numeric>",
      "<optional>",
      "<ostream>",
      "<queue>",
      "<random>",
      "<ranges>",
      "<ratio>",
      "<regex>",
      "<scoped_allocator>",
      "<semaphore>",
      "<set>",
      "<shared_mutex>",
      "<source_location>",
      "<span>",
      "<sstream>",
      "<stack>",
      "<stdexcept>",
      "<stop_token>",
      "<streambuf>",
      "<string>",
      "<string_view>",
      "<strstream>",
      "<syncstream>",
      "<system_error>",
      "<thread>",
      "<tuple>",
      "<typeindex>",
      "<typeinfo>",
      "<type_traits>",
      "<unordered_map>",
      "<unordered_set>",
      "<utility>",
      "<valarray>",
      "<variant>",
      "<vector>",
      "<version>"
    };

    // Table 24 ([tab:headers.cpp.c])
    //
    static const char* std_non_importable[] = {
      "<cassert>",
      "<cctype>",
      "<cerrno>",
      "<cfenv>",
      "<cfloat>",
      "<cinttypes>",
      "<climits>",
      "<clocale>",
      "<cmath>",
      "<csetjmp>",
      "<csignal>",
      "<cstdarg>",
      "<cstddef>",
      "<cstdint>",
      "<cstdio>",
      "<cstdlib>",
      "<cstring>",
      "<ctime>",
      "<cuchar>",
      "<cwchar>",
      "<cwctype>"
    };

    void
    guess_std_importable_headers (const compiler_info& ci,
                                  const dir_paths& sys_hdr_dirs,
                                  importable_headers& hs)
    {
      hs.group_map.emplace (header_group_std, 0);
      hs.group_map.emplace (header_group_std_importable, 0);

      // For better performance we make compiler-specific assumptions.
      //
      // For example, we can assume that all these headers are found in the
      // same header search directory. This is at least the case for GCC's
      // libstdc++.
      //
      // Note also that some headers could be missing. For example, <format>
      // is currently not provided by GCC. Though entering missing headers
      // should be harmless.
      //
      // Plus, a freestanding implementation may only have a subset of such
      // headers (see [compliance]).
      //
      pair<const path, importable_headers::groups>* p;
      auto add_groups = [&p] (bool imp)
      {
        if (imp)
          p->second.push_back (header_group_std_importable); // More specific.

        p->second.push_back (header_group_std);
      };

      if (ci.id.type != compiler_type::gcc)
      {
        for (const char* f: std_importable)
          if ((p = hs.insert_angle (sys_hdr_dirs, f)) != nullptr)
            add_groups (true);

        for (const char* f: std_non_importable)
          if ((p = hs.insert_angle (sys_hdr_dirs, f)) != nullptr)
            add_groups (false);
      }
      else
      {
        // While according to [compliance] a freestanding implementation
        // should provide a subset of headers, including <initializer_list>,
        // there seem to be cases where no headers are provided at all (see GH
        // issue #219). So if we cannot find <initializer_list>, we just skip
        // the whole thing.
        //
        p = hs.insert_angle (sys_hdr_dirs, std_importable[0]);

        if (p != nullptr)
        {
          assert (p != nullptr);

          add_groups (true);

          dir_path d (p->first.directory ());

          auto add_header = [&hs, &d, &p, add_groups] (const char* f, bool imp)
          {
            path fp (d);
            fp.combine (f + 1, strlen (f) - 2, '\0'); // Assuming simple.

            p = &hs.insert_angle (move (fp), f);
            add_groups (imp);
          };

          for (size_t i (1);
               i != sizeof (std_importable) / sizeof (std_importable[0]);
               ++i)
            add_header (std_importable[i], true);

          for (const char* f: std_non_importable)
            add_header (f, false);
        }
      }
    }
  }
}
