// file      : build2/cc/msvc.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cstring> // strcmp()

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/types.hxx>

#include <build2/cc/common.hxx>
#include <build2/cc/module.hxx>

using std::strcmp;

using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

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
        fail << "unable to translate CPU " << cpu << " to /MACHINE";

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

    // Sense whether this is a diagnostics line returning the postion of the
    // NNNN code in XNNNN and npos otherwise.
    //
    size_t
    msvc_sense_diag (const string& l, char f)
    {
      size_t p (l.find (':'));

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

      return p;
    }

    // Filter cl.exe and link.exe noise.
    //
    void
    msvc_filter_cl (ifdstream& is, const path& src)
    {
      // While it appears VC always prints the source name (event if the
      // file does not exist), let's do a sanity check. Also handle the
      // command line errors/warnings which come before the file name.
      //
      for (string l; !eof (getline (is, l)); )
      {
        if (l != src.leaf ().string ())
        {
          diag_stream_lock () << l << endl;

          if (msvc_sense_diag (l, 'D') != string::npos)
            continue;
        }

        break;
      }
    }

    void
    msvc_filter_link (ifdstream& is, const file& t, otype lt)
    {
      // Filter lines until we encounter something we don't recognize. We also
      // have to assume the messages can be translated.
      //
      for (string l; getline (is, l); )
      {
        // "   Creating library foo\foo.dll.lib and object foo\foo.dll.exp"
        //
        // This can also appear when linking executables if any of the object
        // files export any symbols.
        //
        if (l.compare (0, 3, "   ") == 0)
        {
          // Use the actual import library name if this is a library (since we
          // override this name) and the executable name otherwise (by default
          // .lib/.exp are named by replacing the .exe extension).
          //
          path i (
            lt == otype::s
            ? t.member->as<file> ().path ().leaf ()
            : t.path ().leaf ().base () + ".lib");

          if (l.find (i.string ())                  != string::npos &&
              l.find (i.base ().string () + ".exp") != string::npos)
            continue;
        }

        // /INCREMENTAL causes linker to sometimes issue messages but now I
        // can't quite reproduce it.
        //

        diag_stream_lock () << l << endl;
        break;
      }
    }

    // Extract system header search paths from MSVC.
    //
    dir_paths config_module::
    msvc_header_search_paths (const process_path&, scope&) const
    {
      // The compiler doesn't seem to have any built-in paths and all of them
      // come from the INCLUDE environment variable.

      // @@ VC: how are we going to do this? E.g., cl-14 does this internally.
      //    cl.exe /Be prints INCLUDE.
      //
      //    Should we actually bother? INCLUDE is normally used for system
      //    headers and its highly unlikely we will see an imported library
      //    that lists one of those directories in pkg-config Cflags value.
      //    Let's wait and see.
      //
      return dir_paths ();
     }

    // Extract system library search paths from MSVC.
    //
    dir_paths config_module::
    msvc_library_search_paths (const process_path&, scope&) const
    {
      // The linker doesn't seem to have any built-in paths and all of them
      // come from the LIB environment variable.

      // @@ VC: how are we going to do this? E.g., cl-14 does this internally.
      //    cl.exe /Be prints LIB.
      //
      //    Should we actually bother? LIB is normally used for system
      //    libraries and its highly unlikely we will see an explicit import
      //    for a library from one of those directories. Let's wait and see.
      //
      return dir_paths ();
    }

    // Inspect the file and determine if it is static or import library.
    // Return otype::e if it is neither (which we quietly ignore).
    //
    static otype
    library_type (const process_path& ld, const path& l)
    {
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
      const char* args[] = {ld.recall_string (),
                            "/DUMP",               // Must come first.
                            "/NOLOGO",
                            "/ARCHIVEMEMBERS",
                            l.string ().c_str (),
                            nullptr};

      if (verb >= 3)
        print_process (args);

      // Link.exe seem to always dump everything to stdout but just in case
      // redirect stderr to stdout.
      //
      process pr (run_start (ld,
                             args,
                             0     /* stdin */,
                             -1    /* stdout */,
                             false /* error */));

      bool obj (false), dll (false);
      string s;

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

          // The lines we are interested in seem to have this form (though
          // presumably the "Archive member name at" part can be translated):
          //
          // Archive member name at 746: [...]hello.dll[/][ ]*
          // Archive member name at 8C70: [...]hello.lib.obj[/][ ]*
          //
          size_t n (s.size ());

          for (; n != 0 && s[n - 1] == ' '; --n) ; // Skip trailing spaces.

          if (n >= 7) // At least ": X.obj" or ": X.dll".
          {
            --n;

            if (s[n] == '/') // Skip trailing slash if one is there.
              --n;

            n -= 3; // Beginning of extension.

            if (s[n] == '.')
            {
              // Make sure there is ": ".
              //
              size_t p (s.rfind (':', n - 1));

              if (p != string::npos && s[p + 1] == ' ')
              {
                const char* e (s.c_str () + n + 1);

                if (casecmp (e, "obj", 3) == 0)
                  obj = true;

                if (casecmp (e, "dll", 3) == 0)
                  dll = true;
              }
            }
          }
        }
      }
      catch (const io_error&)
      {
        // Presumably the child process failed. Let run_finish() deal with
        // that.
      }

      if (!run_finish (args, pr, false, s))
        return otype::e;

      if (obj && dll)
      {
        warn << l << " looks like hybrid static/import library, ignoring";
        return otype::e;
      }

      if (!obj && !dll)
      {
        warn << l << " looks like empty static or import library, ignoring";
        return otype::e;
      }

      return obj ? otype::a : otype::s;
    }

    template <typename T>
    static T*
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
      timestamp mt (file_mtime (f));

      if (mt != timestamp_nonexistent && library_type (ld, f) == lt)
      {
        // Enter the target.
        //
        T* t;
        common::insert_library (t, name, d, e, exist, trace);

        t->mtime (mt);
        t->path (move (f));

        return t;
      }

      return nullptr;
    }

    liba* common::
    msvc_search_static (const process_path& ld,
                        const dir_path& d,
                        const prerequisite_key& p,
                        bool exist) const
    {
      tracer trace (x, "msvc_search_static");

      liba* r (nullptr);

      auto search = [&r, &ld, &d, &p, exist, &trace] (
        const char* pf, const char* sf) -> bool
      {
        r = msvc_search_library<liba> (
          ld, d, p, otype::a, pf, sf, exist, trace);
        return r != nullptr;
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
        search ("",    "_static") ? r : nullptr;
    }

    libs* common::
    msvc_search_shared (const process_path& ld,
                        const dir_path& d,
                        const prerequisite_key& pk,
                        bool exist) const
    {
      tracer trace (x, "msvc_search_shared");

      libs* s (nullptr);

      auto search = [&s, &ld, &d, &pk, exist, &trace] (
        const char* pf, const char* sf) -> bool
      {
        if (libi* i = msvc_search_library<libi> (
              ld, d, pk, otype::s, pf, sf, exist, trace))
        {
          ulock l (insert_library (s, *pk.tk.name, d, nullopt, exist, trace));

          if (!exist)
          {
            if (l.owns_lock ())
            {
              s->member = i;
              l.unlock ();
            }
            else
              assert (s->member == i);

            // Presumably there is a DLL somewhere, we just don't know where.
            //
            s->mtime (i->mtime ());
            s->path (path ());
          }
        }

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
        search ("",    "dll") ? s : nullptr;
    }
  }
}
