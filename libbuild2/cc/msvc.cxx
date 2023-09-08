// file      : libbuild2/cc/msvc.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cstring> // strcmp()

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>

#include <libbuild2/cc/types.hxx>

#include <libbuild2/cc/common.hxx>
#include <libbuild2/cc/module.hxx>

using std::strcmp;

using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // Translate the target triplet CPU to MSVC CPU (used in directory names,
    // etc).
    //
    const char*
    msvc_cpu (const string& cpu)
    {
      const char* m (cpu == "i386" || cpu == "i686"  ? "x86"   :
                     cpu == "x86_64"                 ? "x64"   :
                     cpu == "arm"                    ? "arm"   :
                     cpu == "arm64"                  ? "arm64" :
                     nullptr);

      if (m == nullptr)
        fail << "unable to translate target triplet CPU " << cpu
             << " to MSVC CPU";

      return m;
    }

    // Translate the target triplet CPU to lib.exe/link.exe /MACHINE option.
    //
    const char*
    msvc_machine (const string& cpu)
    {
      const char* m (cpu == "i386" || cpu == "i686"  ? "/MACHINE:x86"   :
                     cpu == "x86_64"                 ? "/MACHINE:x64"   :
                     cpu == "arm"                    ? "/MACHINE:ARM"   :
                     cpu == "arm64"                  ? "/MACHINE:ARM64" :
                     nullptr);

      if (m == nullptr)
        fail << "unable to translate target triplet CPU " << cpu
             << " to /MACHINE";

      return m;
    }

    // Sanitize cl.exe options.
    //
    void
    msvc_sanitize_cl (cstrings& args)
    {
      // VC is trying to be "helpful" and warn about one command line option
      // overriding another. For example:
      //
      // cl : Command line warning D9025 : overriding '/W1' with '/W2'
      //
      // So we have to sanitize the command line and suppress duplicates of
      // certain options.
      //
      // Note also that it is theoretically possible we will treat an option's
      // argument as an option. Oh, well, nobody is perfect in the Microsoft
      // land.

      // We want to keep the last option seen at the position (relative to
      // other options) that it was encountered. If we were to iterate forward
      // and keep positions of the enountered options, then we would have had
      // to adjust some of them once we remove a duplicate. So instead we are
      // going to iterate backwards, in which case we don't even need to keep
      // positions, just flags. Note that args[0] is cl.exe itself in which we
      // are conveniently not interested.
      //
      bool W (false); // /WN /Wall /w

      for (size_t i (args.size () - 1); i != 0; --i)
      {
        auto erase = [&args, &i] ()
        {
          args.erase (args.begin () + i);
        };

        const char* a (args[i]);

        if (*a != '/' && *a != '-') // Not an option.
          continue;

        ++a;

        // /WN /Wall /w
        //
        if ((a[0] == 'W' && digit (a[1]) && a[2] == '\0') || // WN
            (a[0] == 'W' && strcmp (a + 1, "all") == 0)   || // Wall
            (a[0] == 'w' && a[1] == '\0'))                   // w
        {
          if (W)
            erase ();
          else
            W = true;
        }
      }
    }

    // Sense whether this is a diagnostics line returning in the first half of
    // pair the position of the NNNN code in XNNNN and npos otherwise. If the
    // first half is not npos then the second half is the start of the last
    // path component before first `:`.
    //
    // foo\bar.h: fatal error C1083: ...
    //
    pair<size_t, size_t>
    msvc_sense_diag (const string& l, char f)
    {
      size_t c (l.find (": ")), p (c);

      // Note that while the C-numbers seems to all be in the ' CNNNN:' form,
      // the D ones can be ' DNNNN :', for example:
      //
      // cl : Command line warning D9025 : overriding '/W3' with '/W4'
      //
      for (size_t n (l.size ());
           p != string::npos;
           p = ++p != n ? l.find_first_of (": ", p) : string::npos)
      {
        if (p > 5 &&
            l[p - 6] == ' '  &&
            l[p - 5] == f    &&
            digit (l[p - 4]) &&
            digit (l[p - 3]) &&
            digit (l[p - 2]) &&
            digit (l[p - 1]))
        {
          p -= 4; // Start of the error code.
          break;
        }
      }

      if (p != string::npos)
      {
        c = path::traits_type::rfind_separator (l, c);
        c = c != string::npos ? c + 1 : 0;
      }

      return make_pair (p, c);
    }

    // Filter cl.exe and link.exe noise.
    //
    // Note: must be followed with the dbuf.read() call.
    //
    void
    msvc_filter_cl (diag_buffer& dbuf, const path& src)
    try
    {
      // While it appears VC always prints the source name (event if the
      // file does not exist), let's do a sanity check. Also handle the
      // command line errors/warnings which come before the file name.
      //
      for (string l; !eof (getline (dbuf.is, l)); )
      {
        if (l != src.leaf ().string ())
        {
          dbuf.write (l, true /* newline */);

          if (msvc_sense_diag (l, 'D').first != string::npos)
            continue;
        }

        break;
      }
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << dbuf.args0 << " stderr: " << e;
    }

    void
    msvc_filter_link (diag_buffer& dbuf, const file& t, otype lt)
    try
    {
      // Filter lines until we encounter something we don't recognize. We also
      // have to assume the messages can be translated.
      //
      for (string l; getline (dbuf.is, l); )
      {
        // "   Creating library foo\foo.dll.lib and object foo\foo.dll.exp"
        //
        // This can also appear when linking executables if any of the object
        // files export any symbols.
        //
        if (l.compare (0, 3, "   ") == 0)
        {
          // Use the actual import library name if this is a library (since we
          // override this name) and the executable name otherwise (we pass
          // /IMPLIB with .lib appended to the .exe extension).
          //
          path i (
            lt == otype::s
            ? find_adhoc_member<libi> (t)->path ().leaf ()
            : t.path ().leaf () + ".lib");

          if (l.find (i.string ())                  != string::npos &&
              l.find (i.base ().string () + ".exp") != string::npos)
            continue;
        }

        // /INCREMENTAL causes linker to sometimes issue messages but now I
        // can't quite reproduce it.

        dbuf.write (l, true /* newline */);
        break;
      }
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << dbuf.args0 << " stderr: " << e;
    }

    void
    msvc_extract_header_search_dirs (const strings& v, dir_paths& r)
    {
      for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
      {
        const string& o (*i);

        dir_path d;
        try
        {
          // -I can either be in the "-Ifoo" or "-I foo" form. For MSVC it can
          // also be /I. And from 16.10 it can also be /external:I.
          //
          size_t p;
          if ((o[0] == '-' || o[0] == '/') &&
              (p = (o[1] == 'I'                          ?  2 :
                    o.compare (1, 10, "external:I") == 0 ? 11 : 0)) != 0)
          {
            if (o.size () == p)
            {
              if (++i == e)
                break; // Let the compiler complain.

              d = dir_path (*i);
            }
            else
              d = dir_path (o, p, string::npos);
          }
          else
            continue;

          // Ignore relative paths. Or maybe we should warn?
          //
          if (d.relative ())
            continue;

          d.normalize ();
        }
        catch (const invalid_path& e)
        {
          fail << "invalid directory '" << e.path << "'" << " in option '"
               << o << "'";
        }

        r.push_back (move (d));
      }
    }

    void
    msvc_extract_library_search_dirs (const strings& v, dir_paths& r)
    {
      for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
      {
        const string& o (*i);

        dir_path d;
        try
        {
          // /LIBPATH:<dir> (case-insensitive).
          //
          if ((o[0] == '/' || o[0] == '-') &&
              icasecmp (o.c_str () + 1, "LIBPATH:", 8) == 0)
            d = dir_path (o, 9, string::npos);
          else
            continue;

          // Ignore relative paths. Or maybe we should warn?
          //
          if (d.relative ())
            continue;

          d.normalize ();
        }
        catch (const invalid_path& e)
        {
          fail << "invalid directory '" << e.path << "'" << " in option '"
               << o << "'";
        }

        r.push_back (move (d));
      }
    }

    // Parse semicolon-separated list of search directories (from INCLUDE/LIB
    // environment variables).
    //
    static void
    parse_search_dirs (const string& v, dir_paths& r, const char* what)
    {
      for (size_t b (0), e (0); next_word (v, b, e, ';'); )
      {
        string d (v, b, e - b);
        trim (d);

        if (!d.empty ())
        {
          try
          {
            r.push_back (dir_path (move (d)).normalize ());
          }
          catch (const invalid_path&)
          {
            fail << "invalid path '" << d << "' in " << what;
          }
        }
      }
    }

    // Extract system header search paths from MSVC.
    //
    pair<dir_paths, size_t> config_module::
    msvc_header_search_dirs (const compiler_info&, scope& rs) const
    {
      // MSVC doesn't have any built-in paths and all of them either come from
      // the INCLUDE environment variable or are specified explicitly on the
      // command line (we now do this if running out of the command prompt;
      // see guess). Note that this is not used for Clang targeting MSVC (but
      // is for clang-cl).

      // Extract /I and similar paths from the compiler mode.
      //
      dir_paths r;
      msvc_extract_header_search_dirs (cast<strings> (rs[x_mode]), r);
      size_t rn (r.size ());

      // @@ This does not work for our msvc-linux wrappers which set the
      //    environment variable internally. One way to make this work would
      //    be run `cl.exe /Be` which prints INCLUDE and LIB (but not for
      //    clang-cl).
      //
      if (optional<string> v = getenv ("INCLUDE"))
        parse_search_dirs (*v, r, "INCLUDE environment variable");

      return make_pair (move (r), rn);
     }

    // Extract system library search paths from MSVC.
    //
    pair<dir_paths, size_t> config_module::
    msvc_library_search_dirs (const compiler_info&, scope& rs) const
    {
      // MSVC doesn't seem to have any built-in paths and all of them either
      // come from the LIB environment variable or are specified explicitly on
      // the command line (we now do this if running out of the command
      // prompt; see guess). See the header case above for details.

      // Extract /LIBPATH paths from the compiler mode.
      //
      dir_paths r;
      msvc_extract_library_search_dirs (cast<strings> (rs[x_mode]), r);
      size_t rn (r.size ());

      // @@ This does not work for our msvc-linux wrappers (see above for
      //    details).
      //
      if (optional<string> v = getenv ("LIB"))
        parse_search_dirs (*v, r, "LIB environment variable");

      return make_pair (move (r), rn);
    }

    // Inspect the file and determine if it is static or import library.
    // Return otype::e if it is neither (which we quietly ignore).
    //
    static global_cache<otype> library_type_cache;

    static otype
    library_type (const process_path& ld, const path& l)
    {
      string key;
      {
        sha256 cs;
        cs.append (ld.effect_string ());
        cs.append (l.string ());
        key = cs.string ();

        if (const otype* r = library_type_cache.find (key))
          return *r;
      }

      // The are several reasonably reliable methods to tell whether it is a
      // static or import library. One is lib.exe /LIST -- if there aren't any
      // .obj members, then it is most likely an import library (it can also
      // be an empty static library in which case there won't be any members).
      // For an import library /LIST will print a bunch of .dll members.
      //
      // Another approach is dumpbin.exe (link.exe /DUMP) with /ARCHIVEMEMBERS
      // (similar to /LIST) and /LINKERMEMBER (looking for __impl__ symbols or
      // _IMPORT_DESCRIPTOR_).
      //
      // Note also, that apparently it is possible to have a hybrid library.
      //
      // While the lib.exe approach is probably the simplest, the problem is
      // it will require us loading the bin.ar module even if we are not
      // building any static libraries. On the other hand, if we are searching
      // for libraries then we have bin.ld. So we will use the link.exe /DUMP
      // /ARCHIVEMEMBERS.
      //
      // But then we wanted to also support lld-link which does not support
      // the /DUMP option (at least not as of LLVM 10). On the other hand, it
      // turns out that both link.exe and lld-link support the /LIB option
      // which makes them act as lib.exe. So we have started with /DUMP but
      // now switched to /LIB.
      //
      const char* args[] = {ld.recall_string (),
                            "/LIB",    // Must come first.
                            "/NOLOGO",
                            "/LIST",
                            l.string ().c_str (),
                            nullptr};

      if (verb >= 3)
        print_process (args);

      // link.exe always dump everything to stdout (including diagnostics)
      // while lld-link sends diagnostics to stderr so redirect stderr to
      // stdout.
      //
      process pr (run_start (ld,
                             args,
                             0  /* stdin */,
                             -1 /* stdout */,
                             1  /* stderr (to stdout) */));

      bool obj (false), dll (false);
      string s;

      bool io (false);
      try
      {
        ifdstream is (
          move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        while (getline (is, s))
        {
          // Detect the one error we should let through.
          //
          if (s.compare (0, 18, "unable to execute ") == 0)
            break;

          // The lines we are interested in seem to have this form:
          //
          // libhello\hello.lib.obj
          // hello-0.1.0-a.0.19700101000000.dll
          //
          size_t n (s.size ());

          for (; n != 0 && s[n - 1] == ' '; --n) ; // Skip trailing spaces.

          if (n >= 5) // At least "X.obj" or "X.dll".
          {
            n -= 4; // Beginning of extension.

            if (s[n] == '.')
            {
              const char* e (s.c_str () + n + 1);

              if (icasecmp (e, "obj", 3) == 0)
                obj = true;
              else if (icasecmp (e, "dll", 3) == 0)
                dll = true;
            }
          }
        }

        is.close ();
      }
      catch (const io_error&)
      {
        // Presumably the child process failed so let run_finish() deal with
        // that first.
        //
        io = true;
      }

      if (!run_finish_code (args, pr, s, 2 /* verbosity */) || io)
      {
        diag_record dr;
        dr << warn << "unable to detect " << l << " library type, ignoring" <<
          info << "run the following command to investigate" <<
          info; print_process (dr, args);
        return otype::e;
      }

      otype r;
      if (obj != dll)
        r = obj ? otype::a : otype::s;
      else
      {
        if (obj && dll)
          warn << l << " looks like hybrid static/import library, ignoring";

        if (!obj && !dll)
          warn << l << " looks like empty static or import library, ignoring";

        r = otype::e;
      }

      return library_type_cache.insert (move (key), r);
    }

    template <typename T>
    static pair<T*, bool>
    msvc_search_library (const process_path& ld,
                         const dir_path& d,
                         const prerequisite_key& p,
                         otype lt,
                         const char* pfx,
                         const char* sfx,
                         bool exist,
                         tracer& trace)
    {
      // Pretty similar logic to search_library().
      //
      assert (p.scope != nullptr);

      const optional<string>& ext (p.tk.ext);
      const string& name (*p.tk.name);

      // Assemble the file path.
      //
      path f (d);

      if (*pfx != '\0')
      {
        f /= pfx;
        f += name;
      }
      else
        f /= name;

      if (*sfx != '\0')
        f += sfx;

      const string& e (!ext || p.is_a<lib> () // Only for liba/libs.
                       ? string ("lib")
                       : *ext);

      if (!e.empty ())
      {
        f += '.';
        f += e;
      }

      // Check if the file exists and is of the expected type.
      //
      timestamp mt (mtime (f));

      pair<T*, bool> r (nullptr, true);

      if (mt != timestamp_nonexistent)
      {
        if (library_type (ld, f) == lt)
        {
          // Enter the target.
          //
          common::insert_library (
            p.scope->ctx, r.first, name, d, ld, e, exist, trace);
          r.first->path_mtime (move (f), mt);
        }
        else
          r.second = false; // Don't search for binless.
      }

      return r;
    }

    pair<bin::liba*, bool> common::
    msvc_search_static (const process_path& ld,
                        const dir_path& d,
                        const prerequisite_key& p,
                        bool exist) const
    {
      tracer trace (x, "msvc_search_static");

      liba* a (nullptr);
      bool b (true);

      auto search = [&a, &b,  &ld, &d, &p, exist, &trace] (
        const char* pf, const char* sf) -> bool
      {
        pair<liba*, bool> r (msvc_search_library<liba> (
                               ld, d, p, otype::a, pf, sf, exist, trace));

        if (r.first != nullptr)
          a = r.first;
        else if (!r.second)
          b = false;

        return a != nullptr;
      };

      // Try:
      //      foo.lib
      //   libfoo.lib
      //      foolib.lib
      //      foo_static.lib
      //
      return
        search ("",    "")    ||
        search ("lib", "")    ||
        search ("",    "lib") ||
        search ("",    "_static") ? make_pair (a, true) : make_pair (nullptr, b);
    }

    pair<bin::libs*, bool> common::
    msvc_search_shared (const process_path& ld,
                        const dir_path& d,
                        const prerequisite_key& pk,
                        bool exist) const
    {
      tracer trace (x, "msvc_search_shared");

      assert (pk.scope != nullptr);

      libs* s (nullptr);
      bool b (true);

      auto search = [&s, &b, &ld, &d, &pk, exist, &trace] (
        const char* pf, const char* sf) -> bool
      {
        pair<libi*, bool> r (msvc_search_library<libi> (
                               ld, d, pk, otype::s, pf, sf, exist, trace));
        if (r.first != nullptr)
        {
          ulock l (
            insert_library (
              pk.scope->ctx, s, *pk.tk.name, d, ld, nullopt, exist, trace));

          if (!exist)
          {
            libi* i (r.first);

            if (l.owns_lock ())
            {
              s->adhoc_member = i; // We are first.
              l.unlock ();
            }
            else
              assert (find_adhoc_member<libi> (*s) == i);

            // Presumably there is a DLL somewhere, we just don't know where.
            //
            s->path_mtime (path (), i->mtime ());
          }
        }
        else if (!r.second)
          b = false;

        return s != nullptr;
      };

      // Try:
      //      foo.lib
      //   libfoo.lib
      //      foodll.lib
      //
      return
        search ("",    "")    ||
        search ("lib", "")    ||
        search ("",    "dll") ? make_pair (s, true) : make_pair (nullptr, b);
    }
  }
}
