// file      : libbuild2/cc/pkgconfig.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/install/utility.hxx>

#include <libbuild2/bin/target.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/target.hxx>  // pc
#include <libbuild2/cc/utility.hxx>

#include <libbuild2/cc/common.hxx>
#include <libbuild2/cc/pkgconfig.hxx>
#include <libbuild2/cc/compile-rule.hxx>
#include <libbuild2/cc/link-rule.hxx>

using namespace std; // VC16

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // In pkg-config backslashes, spaces, etc are escaped with a backslash.
    //
    // @@ TODO: handle empty values (save as ''?)
    //
    // Note: may contain variable expansions (e.g, ${pcfiledir}) so unclear
    //       if can use quoting.
    //
    static string
    escape (const string& s)
    {
      string r;

      for (size_t p (0);;)
      {
        size_t sp (s.find_first_of (" \\\"'", p));

        if (sp != string::npos)
        {
          r.append (s, p, sp - p);
          r += '\\';
          r += s[sp];
          p = sp + 1;
        }
        else
        {
          r.append (s, p, sp);
          break;
        }
      }

      return r;
    }

    // Resolve metadata value type from type name. Return in the second half
    // of the pair whether this is a dir_path-based type.
    //
    static pair<const value_type*, bool>
    metadata_type (const string& tn)
    {
      bool d (false);
      const value_type* r (nullptr);

      if      (tn == "bool")       r = &value_traits<bool>::value_type;
      else if (tn == "int64")      r = &value_traits<int64_t>::value_type;
      else if (tn == "uint64")     r = &value_traits<uint64_t>::value_type;
      else if (tn == "string")     r = &value_traits<string>::value_type;
      else if (tn == "path")       r = &value_traits<path>::value_type;
      else if (tn == "dir_path")  {r = &value_traits<dir_path>::value_type; d = true;}
      else if (tn == "int64s")     r = &value_traits<int64s>::value_type;
      else if (tn == "uint64s")    r = &value_traits<uint64s>::value_type;
      else if (tn == "strings")    r = &value_traits<strings>::value_type;
      else if (tn == "paths")      r = &value_traits<paths>::value_type;
      else if (tn == "dir_paths") {r = &value_traits<dir_paths>::value_type; d = true;}

      return make_pair (r, d);
    }

    // In order not to complicate the bootstrap procedure with libpkg-config
    // building, exclude functionality that involves reading of .pc files.
    //
#ifndef BUILD2_BOOTSTRAP

    // Try to find a .pc file in the pkgconfig/ subdirectory of libd, trying
    // several names derived from stem. If not found, return false. If found,
    // load poptions, loptions, libs, and modules, set the corresponding
    // *.export.* variables and add prerequisites on targets, and return true.
    // Note that we assume the targets are locked so that all of this is
    // MT-safe.
    //
    // System library search paths (those extracted from the compiler) are
    // passed in top_sysd while the user-provided (via -L) in top_usrd.
    //
    // Note that scope and link order should be "top-level" from the
    // search_library() POV.
    //
    // Also note that the bootstrapped version of build2 will not search for
    // .pc files, always returning false (see above for the reasoning).
    //

    // Derive pkg-config search directories from the specified library search
    // directory passing them to the callback function for as long as it
    // returns false (e.g., not found). Return true if the callback returned
    // true.
    //
    bool common::
    pkgconfig_derive (const dir_path& d, const pkgconfig_callback& f) const
    {
      dir_path pd (d);

      // First always check the pkgconfig/ subdirectory in this library
      // directory. Even on platforms where this is not the canonical place,
      // .pc files of autotools-based packages installed by the user often
      // still end up there.
      //
      if (exists (pd /= "pkgconfig") && f (move (pd)))
        return true;

      // Platform-specific locations.
      //
      if (tsys == "linux-gnu")
      {
        // On Linux (at least on Debain and Fedora) .pc files for header-only
        // libraries often go to /usr/share/pkgconfig/.
        //
        (((pd = d) /= "..") /= "share") /= "pkgconfig";

        if (exists (pd) && f (move (pd)))
          return true;
      }
      else if (tsys == "freebsd")
      {
        // On FreeBSD (but not NetBSD) .pc files go to libdata/pkgconfig/, not
        // lib/pkgconfig/.
        //
        (((pd = d) /= "..") /= "libdata") /= "pkgconfig";

        if (exists (pd) && f (move (pd)))
          return true;
      }

      return false;
    }

    // Search for the .pc files in the pkg-config directories that correspond
    // to the specified library directory. If found, return static (first) and
    // shared (second) library .pc files. If common is false, then only
    // consider our .static/.shared files.
    //
    pair<path, path> common::
    pkgconfig_search (const dir_path& libd,
                      const optional<project_name>& proj,
                      const string& stem,
                      bool common) const
    {
      tracer trace (x, "pkgconfig_search");

      // When it comes to looking for .pc files we have to decide where to
      // search (which directory(ies)) as well as what to search for (which
      // names). Suffix is our ".shared" or ".static" extension.
      //
      auto search_dir = [&proj, &stem] (const dir_path& dir,
                                        const string& sfx) -> path
      {
        path f;

        // See if there is a corresponding .pc file. About half of them are
        // called foo.pc and half libfoo.pc (and one of the pkg-config's
        // authors suggests that some of you should call yours foolib.pc, just
        // to keep things interesting, you know).
        //
        // Given the (general) import in the form <proj>%lib{<stem>}, we will
        // first try lib<stem>.pc, then <stem>.pc. Maybe it also makes sense
        // to try <proj>.pc, just in case. Though, according to pkg-config
        // docs, the .pc file should correspond to a library, not project. But
        // then you get something like zlib which calls it zlib.pc. So let's
        // just do it.
        //
        // And as you think you've covered all the bases, someone decides to
        // play with the case (libXau.* vs xau.pc). So let's also try the
        // lower-case versions of the stem unless we are on a case-insensitive
        // filesystem.
        //
        auto check = [&dir, & sfx, &f] (const string& n)
        {
          f = dir;
          f /= n;
          f += sfx;
          f += ".pc";
          return exists (f);
        };

        if (check ("lib" + stem) || check (stem))
          return f;

#ifndef _WIN32
        string lstem (lcase (stem));

        if (lstem != stem)
        {
          if (check ("lib" + lstem) || check (lstem))
            return f;
        }
#endif

        if (proj)
        {
          if (check (proj->string ()))
            return f;
        }

        return path ();
      };

      // Return false (and so stop the iteration) if a .pc file is found.
      //
      // Note that we rely on the "small function object" optimization here.
      //
      struct data
      {
        path a;
        path s;
        bool common;
      } d {path (), path (), common};

      auto check = [&d, &search_dir] (dir_path&& p) -> bool
      {
        // First look for static/shared-specific files.
        //
        d.a = search_dir (p, ".static");
        d.s = search_dir (p, ".shared");

        if (!d.a.empty () || !d.s.empty ())
          return true;

        // Then the common.
        //
        if (d.common)
          d.a = d.s = search_dir (p, "");

        return !d.a.empty ();
      };

      pair<path, path> r;

      if (pkgconfig_derive (libd, check))
      {
        l6 ([&]{trace << "found " << libd << stem << " in "
                      << (d.a.empty () ? d.a : d.s).directory ();});

        r.first  = move (d.a);
        r.second = move (d.s);
      }

      return r;
    }

    bool common::
    pkgconfig_load (optional<action> act,
                    const scope& s,
                    lib& lt,
                    liba* at,
                    libs* st,
                    const optional<project_name>& proj,
                    const string& stem,
                    const dir_path& libd,
                    const dir_paths& top_sysd,
                    const dir_paths& top_usrd,
                    pair<bool, bool> metaonly) const
    {
      assert (at != nullptr || st != nullptr);

      pair<path, path> p (
        pkgconfig_search (libd, proj, stem, true /* common */));

      if (p.first.empty () && p.second.empty ())
        return false;

      pkgconfig_load (
        act, s, lt, at, st, p, libd, top_sysd, top_usrd, metaonly);
      return true;
    }

    // Action should be absent if called during the load phase. If metaonly is
    // true then only load the metadata.
    //
    void common::
    pkgconfig_load (optional<action> act,
                    const scope& s,
                    lib& lt,
                    liba* at,
                    libs* st,
                    const pair<path, path>& paths,
                    const dir_path& libd,
                    const dir_paths& top_sysd,
                    const dir_paths& top_usrd,
                    pair<bool /* a */, bool /* s */> metaonly) const
    {
      tracer trace (x, "pkgconfig_load");

      assert (at != nullptr || st != nullptr);

      const path& ap (paths.first);
      const path& sp (paths.second);

      assert (!ap.empty () || !sp.empty ());

      const scope& rs (*s.root_scope ());

      const dir_path* sysroot (
        cast_null<abs_dir_path> (rs["config.cc.pkgconfig.sysroot"]));

      // Append -I<dir> or -L<dir> option suppressing duplicates. Also handle
      // the sysroot rewrite.
      //
      auto append_dir = [sysroot] (strings& ops, string&& o)
      {
        char c (o[1]);

        // @@ Should we normalize the path for good measure? But on the other
        //    hand, most of the time when it's not normalized, it will likely
        //    be "consistently-relative", e.g., something like
        //    ${prefix}/lib/../include. I guess let's wait and see for some
        //    real-world examples.
        //
        //    Well, we now support generating relocatable .pc files that have
        //    a bunch of -I${pcfiledir}/../../include and -L${pcfiledir}/.. .
        //
        //    On the other hand, there could be symlinks involved and just
        //    normalize() may not be correct.
        //
        //    Note that we do normalize -L paths in the usrd logic later
        //    (but not when setting as *.export.loptions).

        if (sysroot != nullptr)
        {
          // Notes:
          //
          // - The path might not be absolute (we only rewrite absolute ones).
          //
          // - Do this before duplicate suppression since options in ops
          //   already have the sysroot rewritten.
          //
          // - Check if the path already starts with sysroot since some .pc
          //   files might already be in a good shape (e.g., because they use
          //   ${pcfiledir} to support relocation properly).
          //
          const char* op (o.c_str () + 2);
          size_t on (o.size () - 2);

          if (path_traits::absolute (op, on))
          {
            const string& s (sysroot->string ());

            const char* sp (s.c_str ());
            size_t sn (s.size ());

            if (!path_traits::sub (op, on, sp, sn)) // Already in sysroot.
            {
              // Find the first directory seperator that seperates the root
              // component from the rest of the path (think /usr/include,
              // c:\install\include). We need to replace the root component
              // with sysroot. If there is no separator (say, -Ic:) or the
              // path after the separator is empty (say, -I/), then we replace
              // the entire path.
              //
              size_t p (path_traits::find_separator (o, 2));
              if (p == string::npos || p + 1 == o.size ())
                p = o.size ();

              o.replace (2, p - 2, s);
            }
          }
        }

        for (const string& x: ops)
        {
          if (x.size () > 2 && x[0] == '-' && x[1] == c)
          {
            if (path_traits::compare (x.c_str () + 2, x.size () - 2,
                                      o.c_str () + 2, o.size () - 2) == 0)
              return; // Duplicate.
          }
        }

        ops.push_back (move (o));
      };

      // Extract --cflags and set them as lib?{}:export.poptions returing the
      // pointer to the set value. If [as]pops are not NULL, then only keep
      // options that are present in both.
      //
      auto parse_cflags =[&trace,
                          this,
                          &append_dir] (target& t,
                                        const pkgconfig& pc,
                                        bool la,
                                        const strings* apops = nullptr,
                                        const strings* spops = nullptr)
        -> const strings*
      {
        // Note that we normalize `-[IDU] <arg>` to `-[IDU]<arg>`.
        //
        strings pops;

        char arg ('\0'); // Option with pending argument.
        for (string& o: pc.cflags (la))
        {
          if (arg)
          {
            // Can only be an argument for -I, -D, -U options.
            //
            o.insert (0, 1, arg);
            o.insert (0, 1, '-');

            if (arg == 'I')
              append_dir (pops, move (o));
            else
              pops.push_back (move (o));

            arg = '\0';
            continue;
          }

          size_t n (o.size ());

          // We only keep -I, -D and -U.
          //
          if (n >= 2 &&
              o[0] == '-' && (o[1] == 'I' || o[1] == 'D' || o[1] == 'U'))
          {
            if (n > 2)
            {
              if (o[1] == 'I')
                append_dir (pops, move (o));
              else
                pops.push_back (move (o));
            }
            else
              arg = o[1];
            continue;
          }

          l4 ([&]{trace << "ignoring " << pc.path << " --cflags option "
                        << o;});
        }

        if (arg)
          fail << "argument expected after -" << arg <<
            info << "while parsing pkg-config --cflags " << pc.path;

        if (!pops.empty ())
        {
          auto p (t.vars.insert (c_export_poptions));

          // The only way we could already have this value is if this same
          // library was also imported as a project (as opposed to installed).
          // Unlikely but possible. In this case the values were set by the
          // export stub and we shouldn't touch them.
          //
          if (p.second)
          {
            // If required, only keep common stuff. While removing the entries
            // is not the most efficient way, it is simple.
            //
            if (apops != nullptr || spops != nullptr)
            {
              for (auto i (pops.begin ()); i != pops.end (); )
              {
                if ((apops != nullptr && find (
                       apops->begin (), apops->end (), *i) == apops->end ()) ||
                    (spops != nullptr && find (
                       spops->begin (), spops->end (), *i) == spops->end ()))
                  i = pops.erase (i);
                else
                  ++i;
              }
            }

            p.first = move (pops);
            return &p.first.as<strings> ();
          }
        }

        return nullptr;
      };

      // Parse --libs into loptions/libs (interface and implementation). If
      // ps is not NULL, add each resolved library target as a prerequisite.
      //
      auto parse_libs = [this,
                         &append_dir,
                         act, &s, top_sysd] (target& t,
                                             bool binless,
                                             const pkgconfig& pc,
                                             bool la,
                                             prerequisites* ps)
      {
        // Note that we normalize `-L <arg>` to `-L<arg>`.
        //
        strings lops;
        vector<name> libs;

        // Normally we will have zero or more -L's followed by one or more
        // -l's, with the first one being the library itself, unless the
        // library is binless. But sometimes we may have other linker options,
        // for example, -Wl,... or -pthread. It's probably a bad idea to
        // ignore them. Also, theoretically, we could have just the library
        // name/path. Note that (after some meditation) we consider -pthread
        // a special form of -l.
        //
        // The tricky part, of course, is to know whether what follows after
        // an option we don't recognize is its argument or another option or
        // library. What we do at the moment is stop recognizing just library
        // names (without -l) after seeing an unknown option.
        //
        bool first (true), known (true), have_L (false);

        string self; // The library itself (-l of just name/path).

        char arg ('\0'); // Option with pending argument.
        for (string& o: pc.libs (la))
        {
          if (arg)
          {
            // Can only be an argument for an -L option.
            //
            o.insert (0, 1, arg);
            o.insert (0, 1, '-');
            append_dir (lops, move (o));
            arg = '\0';
            continue;
          }

          size_t n (o.size ());

          // See if this is -L.
          //
          if (n >= 2 && o[0] == '-' && o[1] == 'L')
          {
            if (n > 2)
              append_dir (lops, move (o));
            else
              arg = o[1];
            have_L = true;
            continue;
          }

          // See if that's -l, -pthread, or just the library name/path.
          //
          if ((known && n != 0 && o[0] != '-') ||
              (n > 2 && o[0] == '-' && (o[1] == 'l' || o == "-pthread")))
          {
            // Unless binless, the first one is the library itself, which we
            // skip. Note that we don't verify this and theoretically it could
            // be some other library, but we haven't encountered such a beast
            // yet.
            //
            // What we have enountered (e.g., in the Magick++ library) is the
            // library itself repeated in Libs.private. So now we save it and
            // filter all its subsequent occurences.
            //
            // @@ To be safe we probably shouldn't rely on the position and
            //    filter out all occurrences of the library itself (by name?)
            //    and complain if none were encountered.
            //
            //    Note also that the same situation can occur if we have a
            //    binful library for which we could not find the library
            //    binary and are treating it as binless. We now have a diag
            //    frame around the call to search_library() to help diagnose
            //    such situations.
            //
            if (first)
            {
              first = false;

              if (!binless)
              {
                self = move (o);
                continue;
              }
            }
            else
            {
              if (!binless && o == self)
                continue;
            }

            libs.push_back (name (move (o)));
            continue;
          }

          // Otherwise we assume it is some other loption.
          //
          known = false;
          lops.push_back (move (o));
        }

        if (arg)
          fail << "argument expected after -" << arg <<
            info << "while parsing pkg-config --libs " << pc.path;

        // Space-separated list of escaped library flags.
        //
        auto lflags = [&pc, la] () -> string
        {
          string r;
          for (const string& o: pc.libs (la))
          {
            if (!r.empty ())
              r += ' ';
            r += escape (o);
          }
          return r;
        };

        if (!binless && self.empty ())
          fail << "library expected in '" << lflags () << "'" <<
            info << "while parsing pkg-config --libs " << pc.path;

        // Resolve -lfoo into the library file path using our import installed
        // machinery (i.e., we are going to call search_library() that will
        // probably call us again, and so on).
        //
        // The reason we do it is the link order. For general libraries it
        // shouldn't matter if we imported them via an export stub, direct
        // import installed, or via a .pc file (which we could have generated
        // from the export stub). The exception is "runtime libraries" (which
        // are really the extension of libc or the operating system in case of
        // Windows) such as -lm, -ldl, -lpthread (or its -pthread variant),
        // etc. Those we will detect and leave as -l*.
        //
        // If we managed to resolve all the -l's (sans runtime), then we can
        // omit -L's for a nice and tidy command line.
        //
        bool all (true);
        optional<dir_paths> usrd; // Populate lazily.

        for (auto i (libs.begin ()); i != libs.end (); ++i)
        {
          name& n (*i);
          string& l (n.value);

          if (tclass == "windows")
          {
            // This is a potentially very long and unstable list and we may
            // need a mechanism to extend it on the fly. See issue #59 for one
            // idea.
            //
            auto cmp = [&l] (const char* s, size_t n = string::npos)
            {
              return icasecmp (l.c_str () + 2, s, n) == 0;
            };

            if (l[0] != '-') // e.g., just shell32.lib
              continue;
            else if (cmp ("advapi32")        ||
                     cmp ("authz")           ||
                     cmp ("bcrypt")          ||
                     cmp ("comdlg32")        ||
                     cmp ("crypt32")         ||
                     cmp ("d2d1")            ||
                     cmp ("d3d",   3)        || // d3d*
                     cmp ("dbgeng")          ||
                     cmp ("dbghelp")         ||
                     cmp ("dnsapi")          ||
                     cmp ("dwmapi")          ||
                     cmp ("dwrite")          ||
                     cmp ("dxgi")            ||
                     cmp ("dxguid")          ||
                     cmp ("gdi32")           ||
                     cmp ("glu32")           ||
                     cmp ("imagehlp")        ||
                     cmp ("imm32")           ||
                     cmp ("iphlpapi")        ||
                     cmp ("kernel32")        ||
                     cmp ("mincore")         ||
                     cmp ("mpr")             ||
                     cmp ("msimg32")         ||
                     cmp ("mswsock")         ||
                     cmp ("msxml", 5)        || // msxml*
                     cmp ("netapi32")        ||
                     cmp ("normaliz")        ||
                     cmp ("odbc32")          ||
                     cmp ("ole32")           ||
                     cmp ("oleaut32")        ||
                     cmp ("opengl32")        ||
                     cmp ("powrprof")        ||
                     cmp ("psapi")           ||
                     cmp ("rpcrt4")          ||
                     cmp ("secur32")         ||
                     cmp ("setupapi")        ||
                     cmp ("shell32")         ||
                     cmp ("shlwapi")         ||
                     cmp ("synchronization") ||
                     cmp ("user32")          ||
                     cmp ("userenv")         ||
                     cmp ("uuid")            ||
                     cmp ("version")         ||
                     cmp ("windowscodecs")   ||
                     cmp ("winhttp")         ||
                     cmp ("winmm")           ||
                     cmp ("winspool")        ||
                     cmp ("ws2")             ||
                     cmp ("ws2_32")          ||
                     cmp ("wsock32")         ||
                     cmp ("wtsapi32"))
            {
              if (tsys == "win32-msvc")
              {
                // Translate -l<name> to <name>.lib.
                //
                l.erase (0, 2);
                l += ".lib";
              }
              continue;
            }
            else if (tsys == "mingw32")
            {
              if (l == "-pthread")
                continue;
            }
          }
          else
          {
            // These ones are common/standard/POSIX.
            //
            if (l[0] != '-'      || // e.g., absolute path
                l == "-lm"       ||
                l == "-ldl"      ||
                l == "-lrt"      ||
                l == "-pthread"  ||
                l == "-lpthread")
              continue;

            // Note: these lists are most likely incomplete.
            //
            if (tclass == "linux")
            {
              // Some extras from libc (see libc6-dev) and other places.
              //
              if (l == "-lanl"     ||
                  l == "-lcrypt"   ||
                  l == "-lnsl"     ||
                  l == "-lresolv"  ||
                  l == "-lgcc")
                continue;
            }
            else if (tclass == "macos")
            {
              // Note that Mac OS has libiconv in /usr/lib/ which only comes
              // in the shared variant. So we treat it as system.
              //
              if (l == "-lSystem" ||
                  l == "-liconv")
                continue;
            }
            else if (tclass == "bsd")
            {
              if (l == "-lexecinfo")
                continue;
            }
          }

          // Prepare user search paths by entering the -L paths from the .pc
          // file.
          //
          if (have_L && !usrd)
          {
            usrd = dir_paths ();

            for (const string& o: lops)
            {
              // Note: always in the -L<dir> form (see above).
              //
              if (o.size () > 2 && o[0] == '-' && o[1] == 'L')
              {
                string p (o, 2);

                try
                {
                  dir_path d (move (p));

                  if (d.relative ())
                    fail << "relative -L directory '" << d << "' in '"
                         << lflags () << "'" <<
                      info << "while parsing pkg-config --libs " << pc.path;

                  d.normalize ();
                  usrd->push_back (move (d));
                }
                catch (const invalid_path& e)
                {
                  fail << "invalid -L directory '" << e.path << "' in '"
                       << lflags () << "'" <<
                    info << "while parsing pkg-config --libs " << pc.path;
                }
              }
            }
          }

          // @@ OUT: for now we assume out is undetermined, just like in
          // resolve_library().
          //
          dir_path out;
          string nm (l, 2); // Sans -l.

          prerequisite_key pk {
            nullopt, {&lib::static_type, &out, &out, &nm, nullopt}, &s};

          const target* lt;
          {
            auto df = make_diag_frame (
              [&pc, &l](const diag_record& dr)
              {
                location f (pc.path);
                dr << info (f) << "while resolving pkg-config dependency " << l;
              });

            lt = search_library (act, top_sysd, usrd, pk);
          }

          if (lt != nullptr)
          {
            // We used to pick a member but that doesn't seem right since the
            // same target could be used with different link orders.
            //
            n.dir = lt->dir;
            n.type = lib::static_type.name;
            n.value = lt->name;

            if (!lt->out.empty ())
            {
              n.pair = true;
              i = libs.insert (i + 1, name (lt->out));
            }

            if (ps != nullptr)
              ps->push_back (prerequisite (*lt));
          }
          else
          {
            // If we couldn't find the library, then leave it as -l.
            //
            all = false;

            if (tsys == "win32-msvc")
            {
              // Again, translate -l<name> to <name>.lib.
              //
              l = move (nm += ".lib");
            }
          }
        }

        // If all the -l's resolved and there were no other options, then drop
        // all the -L's. If we have unknown options, then leave them in to be
        // safe.
        //
        if (all && known)
          lops.clear ();

        if (!lops.empty ())
        {
          if (tsys == "win32-msvc")
          {
            // Translate -L to /LIBPATH.
            //
            for (string& o: lops)
            {
              size_t n (o.size ());

              // Note: always in the -L<dir> form (see above).
              //
              if (n > 2 && o[0] == '-' && o[1] == 'L')
              {
                o.replace (0, 2, "/LIBPATH:");
              }
            }
          }

          auto p (t.vars.insert (c_export_loptions));

          if (p.second)
            p.first = move (lops);
        }

        // Set even if empty (export override).
        //
        {
          auto p (t.vars.insert (la ? c_export_impl_libs : c_export_libs));

          if (p.second)
            p.first = move (libs);
        }
      };

      // On Windows pkg-config will escape backslahses in paths. In fact, it
      // may escape things even on non-Windows platforms, for example,
      // spaces. So we use a slightly modified version of next_word().
      //
      // @@ TODO: handle quotes (e.g., empty values; see parse_metadata()).
      //          I wonder what we get here if something is quoted in the
      //          .pc file.
      //
      auto next = [] (const string& s, size_t& b, size_t& e) -> string
      {
        string r;
        size_t n (s.size ());

        if (b != e)
          b = e;

        // Skip leading delimiters.
        //
        for (; b != n && s[b] == ' '; ++b) ;

        if (b == n)
        {
          e = n;
          return r;
        }

        // Find first trailing delimiter while taking care of escapes.
        //
        r = s[b];
        for (e = b + 1; e != n && s[e] != ' '; ++e)
        {
          if (s[e] == '\\')
          {
            if (++e == n)
              fail << "dangling escape in pkg-config output '" << s << "'";
          }

          r += s[e];
        }

        return r;
      };

      // Parse the build2.metadata variable value and, if user is true,
      // extract the user metadata, if any, and set extracted variables on the
      // specified target.
      //
      auto parse_metadata = [&next] (target& t,
                                     pkgconfig& pc,
                                     const string& md,
                                     bool user)
      {
        const location loc (pc.path);

        context& ctx (t.ctx);

        optional<uint64_t> ver;
        optional<string> pfx;

        variable_pool* vp (nullptr); // Resolve lazily.

        string s;
        for (size_t b (0), e (0); !(s = next (md, b, e)).empty (); )
        {
          if (!ver)
          {
            try
            {
              ver = value_traits<uint64_t>::convert (name (s), nullptr);
            }
            catch (const invalid_argument& e)
            {
              fail (loc) << "invalid version in build2.metadata variable: "
                         << e;
            }

            if (*ver != 1)
              fail (loc) << "unexpected metadata version " << *ver;

            if (!user)
              return;

            continue;
          }

          if (!pfx)
          {
            if (s.empty ())
              fail (loc) << "empty variable prefix in build2.metadata varible";

            pfx = s;
            continue;
          }

          // The rest is variable name/type pairs.
          //
          size_t p (s.find ('/'));

          if (p == string::npos)
            fail (loc) << "expected name/type pair instead of '" << s << "'";

          string vn (s, 0, p);
          string tn (s, p + 1);

          optional<string> val (pc.variable (vn));

          if (!val)
            fail (loc) << "metadata variable " << vn << " not set";

          pair<const value_type*, bool> vt (metadata_type (tn));
          if (vt.first == nullptr)
            fail (loc) << "unknown metadata type " << tn;

          names ns;
          for (size_t b (0), e (0); !(s = next (*val, b, e)).empty (); )
          {
            ns.push_back (vt.second
                          ? name (dir_path (move (s)))
                          : name (move (s)));
          }

          // These should be public (qualified) variables so go straight for
          // the public variable pool.
          //
          if (vp == nullptr)
            vp = &ctx.var_pool.rw (); // Load phase if user==true.

          const variable& var (vp->insert (move (vn)));

          value& v (t.assign (var));
          v.assign (move (ns), &var);
          typify (v, *vt.first, &var);
        }

        if (!ver)
          fail (loc) << "version expected in build2.metadata variable";

        if (!pfx)
          return; // No user metadata.

        // Set export.metadata to indicate the presence of user metadata.
        //
        t.assign (ctx.var_export_metadata) = names {
          name (std::to_string (*ver)), name (move (*pfx))};
      };

      // Parse modules, enter them as targets, and add them to the
      // prerequisites.
      //
      auto parse_modules = [&trace, this,
                            &next, &s, &lt] (const pkgconfig& pc,
                                             prerequisites& ps)
      {
        optional<string> val (pc.variable ("cxx.modules"));

        if (!val)
          return;

        string m;
        for (size_t b (0), e (0); !(m = next (*val, b, e)).empty (); )
        {
          // The format is <name>=<path> with `..` used as a partition
          // separator (see pkgconfig_save() for details).
          //
          size_t p (m.find ('='));
          if (p == string::npos ||
              p == 0            || // Empty name.
              p == m.size () - 1)  // Empty path.
            fail << "invalid module information in '" << *val << "'" <<
              info << "while parsing pkg-config --variable=cxx.modules "
                   << pc.path;

          string mn (m, 0, p);
          path mp (m, p + 1, string::npos);

          // Must be absolute but may not be normalized due to a relocatable
          // .pc file. We assume there are no symlink shenanigans that would
          // require realize().
          //
          if (!mp.normalized ())
            mp.normalize ();

          path mf (mp.leaf ());

          // Extract module properties, if any.
          //
          optional<string> pp (pc.variable ("cxx.module_preprocessed." + mn));
          optional<string> se (pc.variable ("cxx.module_symexport." + mn));

          // Replace the partition separator.
          //
          if ((p = mn.find ("..")) != string::npos)
            mn.replace (p, 2, 1, ':');

          // For now there are only C++ modules.
          //
          auto tl (
            s.ctx.targets.insert_locked (
              *x_mod,
              mp.directory (),
              dir_path (),
              mf.base ().string (),
              mf.extension (),
              target_decl::implied,
              trace));

          file& mt (tl.first.as<file> ());

          // If the target already exists, then setting its variables is not
          // MT-safe. So currently we only do it if we have the lock (and thus
          // nobody can see this target yet) verifying that this has already
          // been done otherwise.
          //
          // @@ This is not quite correct, though: this target could already
          //    exist but for a "different purpose" (e.g., it could be used as
          //    a header). Well, maybe it shouldn't.
          //
          // @@ Could setting it in the rule-specific vars help? (But we
          //    are not matching a rule for it.) Note that we are setting
          //    it on the module source, not bmi*{}! So rule-specific vars
          //    don't seem to the answer here.
          //
          if (tl.second.owns_lock ())
          {
            mt.path (move (mp));
            mt.vars.assign (c_module_name) = move (mn);

            // Set module properties. Note that if unspecified we should still
            // set them to their default values since the hosting project may
            // have them set to incompatible values.
            //
            {
              value& v (mt.vars.assign (x_preprocessed)); // NULL
              if (pp)
                v = move (*pp);
            }

            {
              mt.vars.assign (x_symexport) = (se && *se == "true");
            }

            tl.second.unlock ();
          }
          else
          {
            if (!mt.vars[c_module_name])
              fail << "unexpected metadata for module target " << mt <<
                info << "module is expected to have assigned name" <<
                info << "make sure this module is used via " << lt
                   << " prerequisite";
          }

          ps.push_back (prerequisite (mt));
        }
      };

      // Parse importable headers, enter them as targets, and add them to
      // the prerequisites.
      //
      auto parse_headers = [&trace, this,
                            &next, &s, &lt] (const pkgconfig& pc,
                                             const target_type& tt,
                                             const char* lang,
                                             prerequisites& ps)
      {
        string var (string (lang) + ".importable_headers");
        optional<string> val (pc.variable (var));

        if (!val)
          return;

        string h;
        for (size_t b (0), e (0); !(h = next (*val, b, e)).empty (); )
        {
          path hp (move (h));

          // Must be absolute but may not be normalized due to a relocatable
          // .pc file. We assume there are no symlink shenanigans that would
          // require realize().
          //
          if (!hp.normalized ())
            hp.normalize ();

          path hf (hp.leaf ());

          auto tl (
            s.ctx.targets.insert_locked (
              tt,
              hp.directory (),
              dir_path (),
              hf.base ().string (),
              hf.extension (),
              target_decl::implied,
              trace));

          file& ht (tl.first.as<file> ());

          // If the target already exists, then setting its variables is not
          // MT-safe. So currently we only do it if we have the lock (and thus
          // nobody can see this target yet) verifying that this has already
          // been done otherwise.
          //
          if (tl.second.owns_lock ())
          {
            ht.path (move (hp));
            ht.vars.assign (c_importable) = true;
            tl.second.unlock ();
          }
          else
          {
            if (!cast_false<bool> (ht.vars[c_importable]))
              fail << "unexpected metadata for existing header target " << ht <<
                info << "header is expected to be marked importable" <<
                info << "make sure this header is used via " << lt
                   << " prerequisite";
          }

          ps.push_back (prerequisite (ht));
        }
      };

      // Load the information from the pkg-config files.
      //
      pkgconfig apc;
      pkgconfig spc;

      // Create the .pc files search directory list.
      //
      dir_paths pc_dirs;

      // Note that we rely on the "small function object" optimization here.
      //
      auto add_pc_dir = [&trace, &pc_dirs] (dir_path&& d) -> bool
      {
        // Suppress duplicated.
        //
        if (find (pc_dirs.begin (), pc_dirs.end (), d) == pc_dirs.end ())
        {
          l6 ([&]{trace << "search path " << d;});
          pc_dirs.emplace_back (move (d));
        }

        return false;
      };

      pkgconfig_derive (libd, add_pc_dir);
      for (const dir_path& d: top_usrd) pkgconfig_derive (d, add_pc_dir);
      for (const dir_path& d: top_sysd) pkgconfig_derive (d, add_pc_dir);

      bool pa (at != nullptr && !ap.empty ());
      if (pa || sp.empty ())
        apc = pkgconfig (ap, pc_dirs, sys_lib_dirs, sys_hdr_dirs);

      bool ps (st != nullptr && !sp.empty ());
      if (ps || ap.empty ())
        spc = pkgconfig (sp, pc_dirs, sys_lib_dirs, sys_hdr_dirs);

      // Load the user metadata if we are in the load phase. Otherwise just
      // determine if we have metadata.
      //
      // Note also that we are not failing here if the metadata was requested
      // but not present (potentially only partially) letting the caller
      // (i.e., the import machinery) verify that the export.metadata was set
      // on the target being imported. This would also allow supporting
      // optional metadata.
      //
      bool apc_meta (false);
      bool spc_meta (false);
      if (!act)
      {
        // We can only do it during the load phase.
        //
        assert (lt.ctx.phase == run_phase::load);

        pkgconfig& ipc (ps ? spc : apc); // As below.

        // Since it's not easy to say if things are the same, we load a copy
        // into the group and each member, if any.
        //
        // @@ TODO: check if already loaded? Don't we have the same problem
        //    below with reloading the rest for lt? What if we passed NULL
        //    in this case (and I suppose another bool in metaonly)?
        //
        if (optional<string> md = ipc.variable ("build2.metadata"))
          parse_metadata (lt, ipc, *md, true);

        if (pa)
        {
          if (optional<string> md = apc.variable ("build2.metadata"))
          {
            parse_metadata (*at, apc, *md, true);
            apc_meta = true;
          }
        }

        if (ps)
        {
          if (optional<string> md = spc.variable ("build2.metadata"))
          {
            parse_metadata (*st, spc, *md, true);
            spc_meta = true;
          }
        }

        // If we only need metadata, then we are done.
        //
        if (at != nullptr && metaonly.first)
        {
          pa = false;
          at = nullptr;
        }

        if (st != nullptr && metaonly.second)
        {
          ps = false;
          st = nullptr;
        }

        if (at == nullptr && st == nullptr)
          return;
      }
      else
      {
        if (pa)
        {
          if (optional<string> md = apc.variable ("build2.metadata"))
          {
            parse_metadata (*at, apc, *md, false);
            apc_meta = true;
          }
        }

        if (ps)
        {
          if (optional<string> md = spc.variable ("build2.metadata"))
          {
            parse_metadata (*st, spc, *md, false);
            spc_meta = true;
          }
        }
      }

      // Sort out the interface dependencies (which we are setting on lib{}).
      // If we have the shared .pc variant, then we use that.  Otherwise --
      // static but extract without the --static option (see also the saving
      // logic).
      //
      pkgconfig& ipc (ps ? spc : apc); // Interface package info.
      bool ipc_meta (ps ? spc_meta : apc_meta);

      // For now we only populate prerequisites for lib{}. To do it for
      // liba{} would require weeding out duplicates that are already in
      // lib{}.
      //
      // Currently, this information is only used by the modules machinery to
      // resolve module names to module files (but we cannot only do this if
      // modules are enabled since the same installed library can be used by
      // multiple builds).
      //
      prerequisites prs;

      parse_libs (
        lt,
        (ps ? st->mtime () : at->mtime ()) == timestamp_unreal /* binless */,
        ipc,
        false,
        &prs);

      const strings* apops (nullptr);
      if (pa)
      {
        apops = parse_cflags (*at, apc, true);
        parse_libs (*at, at->path ().empty (), apc, true, nullptr);
      }

      const strings* spops (nullptr);
      if (ps)
        spops = parse_cflags (*st, spc, false);

      // Also set common poptions for the group. In particular, this makes
      // sure $lib_poptions() in the "common interface" mode works for the
      // installed libraries.
      //
      // Note that if there are no poptions set for either, then we cannot
      // possibly have a common subset.
      //
      if (apops != nullptr || spops != nullptr)
        parse_cflags (lt, ipc, false, apops, spops);

      // @@ TODO: we can now load cc.type if there is metadata (but need to
      //          return this rather than set, see search_library() for
      //          details).

      // Load the bin.whole flag (whole archive).
      //
      if (at != nullptr && (pa ? apc_meta : spc_meta))
      {
        // Note that if unspecified we leave it unset letting the consumer
        // override it, if necessary (see the bin.lib lookup semantics for
        // details).
        //
        if (optional<string> v = (pa ? apc : spc).variable ("bin.whole"))
        {
          at->vars.assign ("bin.whole") = (*v == "true");
        }
      }

      // For now we assume static and shared variants export the same set of
      // modules/importable headers. While technically possible, having
      // different sets will most likely lead to all sorts of complications
      // (at least for installed libraries) and life is short.
      //
      if (modules && ipc_meta)
      {
        parse_modules (ipc, prs);

        // We treat headers outside of any project as C headers (see
        // enter_header() for details).
        //
        parse_headers (ipc, h::static_type /* **x_hdrs */, x, prs);
        parse_headers (ipc, h::static_type, "c", prs);
      }

      assert (!lt.has_prerequisites ());
      if (!prs.empty ())
        lt.prerequisites (move (prs));
    }

#else

    pair<path, path> common::
    pkgconfig_search (const dir_path&,
                      const optional<project_name>&,
                      const string&,
                      bool) const
    {
      return pair<path, path> ();
    }

    bool common::
    pkgconfig_load (optional<action>,
                    const scope&,
                    lib&,
                    liba*,
                    libs*,
                    const optional<project_name>&,
                    const string&,
                    const dir_path&,
                    const dir_paths&,
                    const dir_paths&,
                    pair<bool, bool>) const
    {
      return false;
    }

    void common::
    pkgconfig_load (optional<action>,
                    const scope&,
                    lib&,
                    liba*,
                    libs*,
                    const pair<path, path>&,
                    const dir_path&,
                    const dir_paths&,
                    const dir_paths&,
                    pair<bool, bool>) const
    {
      assert (false); // Should never be called.
    }

#endif

    // If common is true, generate a "best effort" (i.e., not guaranteed to be
    // sufficient in all cases) common .pc file by ignoring any static/shared-
    // specific poptions and splitting loptions/libs into Libs/Libs.private.
    // Note that if both static and shared are being installed, the common
    // file must be generated based on the static library to get accurate
    // Libs.private.
    //
    // The other things that we omit from the common variant are -l options
    // for binless libraries (so that it's usable from other build systems) as
    // well as metadata (which could become incomplete due the previous
    // omissions; for example, importable headers metadata).
    //
    void link_rule::
    pkgconfig_save (action a,
                    const file& l,
                    bool la,
                    bool common,
                    bool binless) const
    {
      tracer trace (x, "pkgconfig_save");

      context& ctx (l.ctx);

      const scope& bs (l.base_scope ());
      const scope& rs (*bs.root_scope ());

      auto* t (find_adhoc_member<pc> (l, (common ? pc::static_type  :
                                          la     ? pca::static_type :
                                          /*    */ pcs::static_type)));
      assert (t != nullptr);

      const path& p (t->path ());

      // If we are uninstalling, skip regenerating the file if it already
      // exists (I think we could have skipped this even if it doesn't exist,
      // but let's keep things close to the install case).
      //
      if (ctx.current_action ().outer_operation () == uninstall_id)
      {
        if (exists (p))
          return;
      }

      // This is the lib{} group if we are generating the common file and the
      // target itself otherwise.
      //
      const target& g (common ? *l.group : l);

      // By default we assume things go into install.{include, lib}.
      //
      // If include.lib does not resolve, then assume this is update-for-
      // install without actual install and remove the file if it exists.
      //
      // @@ Shouldn't we use target's install value rather than install.lib
      //    in case it gets installed into a custom location? I suppose one
      //    can now use cc.pkgconfig.lib to customize this.
      //
      using install::resolve_dir;

      small_vector<dir_path, 1> ldirs;

      if (const dir_paths* ds = cast_null<dir_paths> (g[c_pkgconfig_lib]))
      {
        for (const dir_path& d: *ds)
        {
          bool f (ldirs.empty ());

          ldirs.push_back (resolve_dir (g, d, {}, !f /* fail_unknown */));

          if (f && ldirs.back ().empty ())
            break;
        }
      }
      else
        ldirs.push_back (resolve_dir (g,
                                      cast<dir_path> (g["install.lib"]),
                                      {},
                                      false /* fail_unknown */));

      if (!ldirs.empty () && ldirs.front ().empty ())
      {
        rmfile (ctx, p, 3 /* verbosity */);
        return;
      }

      small_vector<dir_path, 1> idirs;

      if (const dir_paths* ds = cast_null<dir_paths> (g[c_pkgconfig_include]))
      {
        for (const dir_path& d: *ds)
          idirs.push_back (resolve_dir (g, d));
      }
      else
        idirs.push_back (resolve_dir (g,
                                      cast<dir_path> (g["install.include"])));

      // Note that generation can take some time if we have a large number of
      // prerequisite libraries.
      //
      if (verb >= 2)
        text << "cat >" << p;
      else if (verb)
        print_diag ("pc", g, *t);

      if (ctx.dry_run)
        return;

      // See if we should be generating a relocatable .pc file and if so get
      // its installation location. The plan is to make all absolute paths
      // that we write relative to this location and prefix them with the
      // built-in ${pcfiledir} variable (which supported by everybody: the
      // original pkg-config, pkgconf, and our libpkg-config library).
      //
      dir_path rel_base;
      if (cast_false<bool> (rs["install.relocatable"]))
      {
        path f (install::resolve_file (*t));
        if (!f.empty ()) // Shouldn't happen but who knows.
          rel_base = f.directory ();
      }

      // Note: reloc_*path() expect absolute and normalized paths.
      //
      // Note also that reloc_path() can be used on dir_path to get the path
      // without the trailing slash.
      //
      auto reloc_path = [&rel_base,
                         s = string ()] (const path& p,
                                         const char* what) mutable
        -> const string&
      {
        if (rel_base.empty ())
          return p.string ();

        try
        {
          s = p.relative (rel_base).string ();
        }
        catch (const invalid_path&)
        {
          fail << "unable to make " << what << " path " << p << " relative to "
               << rel_base;
        }

        if (!s.empty ()) s.insert (0, 1, path_traits::directory_separator);
        s.insert (0, "${pcfiledir}");
        return s;
      };

      auto reloc_dir_path = [&rel_base,
                             s = string ()] (const dir_path& p,
                                             const char* what) mutable
        -> const string&
      {
        if (rel_base.empty ())
          return (s = p.representation ());

        try
        {
          s = p.relative (rel_base).representation ();
        }
        catch (const invalid_path&)
        {
          fail << "unable to make " << what << " path " << p << " relative to "
               << rel_base;
        }

        if (!s.empty ()) s.insert (0, 1, path_traits::directory_separator);
        s.insert (0, "${pcfiledir}");
        return s;
      };

      auto_rmfile arm (p);

      try
      {
        ofdstream os (p);

        {
          const project_name& n (project (rs));

          if (n.empty ())
            fail << "no project name in " <<  rs;

          lookup vl (rs.vars[ctx.var_version]);
          if (!vl)
            fail << "no version variable in project " << n <<
              info << "while generating " << p;

          // When comparing versions, pkg-config uses RPM semantics, which is
          // basically comparing each all-digit/alpha fragments in order.
          // This means, for example, a semver with a pre-release will be
          // compared incorrectly (pre-release will be greater than the final
          // version). We could detect if this project uses stdver and chop
          // off any pre-release information (so, essentially only saving the
          // major.minor.patch part). But that means such .pc files will
          // contain inaccurate version information. And seeing that we don't
          // recommend using pkg-config (rather primitive) package dependency
          // support, having complete version information for documentation
          // seems more important.
          //
          // @@ Maybe still makes sense to only save version.project_id?
          //
          const string& v (cast<string> (vl));

          os << "Name: " << n << endl;
          os << "Version: " << v << endl;

          // This one is required so make something up if unspecified.
          //
          os << "Description: ";
          if (const string* s = cast_null<string> (rs[ctx.var_project_summary]))
            os << *s << endl;
          else
            os << n << ' ' << v << endl;

          if (const string* u = cast_null<string> (rs[ctx.var_project_url]))
            os << "URL: " << *u << endl;
        }

        auto save_poptions = [&g, &os] (const variable& var)
        {
          if (const strings* v = cast_null<strings> (g[var]))
          {
            for (auto i (v->begin ()); i != v->end (); ++i)
            {
              const string& o (*i);

              // Filter out -I (both -I<dir> and -I <dir> forms).
              //
              if (o[0] == '-' && o[1] == 'I')
              {
                if (o.size () == 2)
                  ++i;

                continue;
              }

              os << ' ' << escape (o);
            }
          }
        };

        // Given a library target, return its -l-style library name.
        //
        auto save_library_target = [this] (const file& l) -> string
        {
          // If available (it may not, in case of import-installed libraris),
          // use the .pc file name to derive the -l library name (in case of
          // the shared library, l.path() may contain version).
          //
          string n;

          auto strip_lib = [&n] ()
          {
            if (n.size () > 3 &&
                path::traits_type::compare (n.c_str (), 3, "lib", 3) == 0)
              n.erase (0, 3);
          };

          if (auto* t = find_adhoc_member<pc> (l))
          {
            // We also want to strip the lib prefix unless it is part of the
            // target name while keeping custom library prefix/suffix, if any.
            //
            n = t->path ().leaf ().base ().base ().string ();

            if (path::traits_type::compare (n.c_str (), n.size (),
                                       l.name.c_str (), l.name.size ()) != 0)
              strip_lib ();
          }
          else
          {
            const path& p (l.path ());

            if (p.empty ()) // Binless.
            {
              // For a binless library the target name is all it can possibly
              // be.
              //
              n = l.name;
            }
            else
            {
              // Derive -l-name from the file name in a fuzzy, platform-
              // specific manner.
              //
              n = p.leaf ().base ().string ();

              if (cclass != compiler_class::msvc)
                strip_lib ();
            }
          }

          return "-l" + n;
        };

        // Given a (presumably) compiler-specific library name, return its
        // -l-style library name.
        //
        auto save_library_name = [this] (const string& n) -> string
        {
          if (tsys == "win32-msvc")
          {
            // Translate <name>.lib to -l<name>.
            //
            size_t p (path::traits_type::find_extension (n));

            if (p != string::npos && icasecmp (n.c_str () + p + 1, "lib") == 0)
            {
              return "-l" + string (n, 0, p);
            }

            // Fall through and return as is.
          }

          return n;
        };

        // Cflags.
        //
        os << "Cflags:";
        for (const dir_path& d: idirs)
          os << " -I" << escape (reloc_path (d, "header search"));
        save_poptions (x_export_poptions);
        save_poptions (c_export_poptions);
        os << endl;

        // Libs.
        //
        // While we generate split shared/static .pc files, in case of static
        // we still want to sort things out into Libs/Libs.private. This is
        // necessary to distinguish between interface and implementation
        // dependencies if we don't have the shared variant (see the load
        // logic for details). And also for the common .pc file, naturally.
        //
        {
          os << "Libs:";

          // While we don't need it for a binless library itselt, it may be
          // necessary to resolve its binful dependencies.
          //
          for (const dir_path& d: ldirs)
            os << " -L" << escape (reloc_path (d, "library search"));

          // Now process ourselves as if we were being linked to something (so
          // pretty similar to link_rule::append_libraries()). We also reuse
          // the link_rule's machinery to suppress duplicates.
          //
          appended_libraries ls;
          strings args;
          bool priv (false);

          struct data
          {
            ofdstream&           os;
            appended_libraries*  pls; // Previous.
            appended_libraries*  ls;  // Current.
            strings&             args;
            bool                 common;
          } d {os, nullptr, &ls, args, common};

          auto imp = [&priv] (const target&, bool la) {return priv && la;};

          auto lib = [&d, &save_library_target, &save_library_name] (
            const target* const* lc,
            const small_vector<reference_wrapper<const string>, 2>& ns,
            lflags,
            const string*,
            bool)
          {
            const file* l (lc != nullptr ? &(*lc)->as<file> () : nullptr);

            // Suppress duplicates from the previous run (Libs/Libs.private
            // split).
            //
            if (d.pls != nullptr)
            {
              // Doesn't feel like we can prune here: we may have seen this
              // interface library but not its implementation dependencies.
              //
              if ((l != nullptr
                   ? d.pls->find (*l)
                   : d.pls->find (ns)) != nullptr)
                return true;
            }

            // Suppress duplicates (see append_libraries() for details).
            //
            // Note that we use the original name for duplicate tracking.
            //
            appended_library* al (l != nullptr
                                  ? &d.ls->append (*l, d.args.size ())
                                  : d.ls->append (ns, d.args.size ()));

            if (al != nullptr && al->end != appended_library::npos)
            {
              d.ls->hoist (d.args, *al);
              return true;
            }

            if (l != nullptr)
            {
              if (l->is_a<libs> () || l->is_a<liba> ()) // See through libux.
              {
                // Omit binless libraries from the common .pc file (see
                // above).
                //
                // Note that in this case we still want to recursively
                // traverse such libraries since they may still link to some
                // non-binless system libraries (-lm, etc).
                //
                if (!d.common || !l->path ().empty ())
                  d.args.push_back (save_library_target (*l));
              }
            }
            else
            {
              // Something "system'y", save as is.
              //
              for (const string& n: ns)
                d.args.push_back (save_library_name (n));
            }

            if (al != nullptr)
              al->end = d.args.size (); // Close.

            return true;
          };

          auto opt = [&d] (const target& lt, const string&, bool, bool)
          {
            const file& l (lt.as<file> ());

            //@@ TODO: should we filter -L similar to -I?
            //@@ TODO: how will the Libs/Libs.private work?
            //@@ TODO: remember to use reloc_*() and escape().

            if (d.pls != nullptr && d.pls->find (l) != nullptr)
              return true;

            // See link_rule::append_libraries().

            if (d.ls->append (l, d.args.size ()).end != appended_library::npos)
              return true;

            return true;
          };

          // Pretend we are linking an executable using what would be normal,
          // system-default link order.
          //
          linfo li {otype::e, la ? lorder::a_s : lorder::s_a};

          library_cache lib_cache;
          process_libraries (a, bs, li, sys_lib_dirs,
                             l, la, 0, // Link flags.
                             imp, lib, opt,
                             !binless /* self */,
                             false /* proc_opt_group */, // @@ !priv?
                             &lib_cache);

          for (const string& a: args)
            os << ' ' << a;
          os << endl;

          if (la)
          {
            os << "Libs.private:";

            args.clear ();
            priv = true;

            // Use previous appended_libraries to weed out entries that are
            // already in Libs.
            //
            appended_libraries als;
            d.pls = d.ls;
            d.ls = &als;

            process_libraries (a, bs, li, sys_lib_dirs,
                               l, la, 0, // Link flags.
                               imp, lib, opt,
                               false /* self */,
                               false /* proc_opt_group */, // @@ !priv?
                               &lib_cache);

            for (const string& a: args)
              os << ' ' << a;
            os << endl;

            // See also bin.whole below.
          }
        }

        // Save metadata unless this is the common .pc file (see above).
        //
        if (common)
        {
          os.close ();
          arm.cancel ();
          return;
        }

        // The build2.metadata variable is a general indication of the
        // metadata being present. Its value is the metadata version
        // optionally followed by the user metadata variable prefix and
        // variable list (see below for details). Having only the version
        // indicates the absense of user metadata.
        //
        // See if we have the user metadata.
        //
        lookup um (g[ctx.var_export_metadata]); // Target visibility.

        if (um && !um->empty ())
        {
          const names& ns (cast<names> (um));

          // First verify the version.
          //
          uint64_t ver;
          try
          {
            // Note: does not change the passed name.
            //
            ver = value_traits<uint64_t>::convert (
              ns[0], ns[0].pair ? &ns[1] : nullptr);
          }
          catch (const invalid_argument& e)
          {
            fail << "invalid metadata version in library " << g << ": " << e
                 << endf;
          }

          if (ver != 1)
            fail << "unexpected metadata version " << ver << " in library "
                 << g;

          // Next verify the metadata variable prefix.
          //
          if (ns.size () != 2 || !ns[1].simple ())
            fail << "invalid metadata variable prefix in library " << g;

          const string& pfx (ns[1].value);

          // Now find all the target-specific variables with this prefix.
          //
          // If this is the common .pc file, then we only look in the group.
          // Otherwise, in the member and the group.
          //
          // To allow setting different values for the for-install and
          // development build cases (required when a library comes with
          // additional "assets"), we recognize the special .for_install
          // variable name suffix: if there is a both <prefix>.<name> and
          // <prefix>.<name>.for_install variables, then here we take the
          // value from the latter. Note that we don't consider just
          // <prefix>.for_install as special (so it's available to the user).
          //
          // We only expect a handful of variables so let's use a vector and
          // linear search instead of a map.
          //
          struct binding
          {
            const string*  name; // Name to be saved (without .for_install).
            const variable* var; // Actual variable (potentially .for_install).
            const value*    val; // Actual value.
          };
          vector<binding> vars;

          auto append = [&l, &pfx, &vars,
                         tmp = string ()] (const target& t, bool dup) mutable
          {
            for (auto p (t.vars.lookup_namespace (pfx));
                 p.first != p.second;
                 ++p.first)
            {
              const variable* var (&p.first->first.get ());

              // Handle .for_install.
              //
              // The plan is as follows: if this is .for_install, then just
              // verify we also have the value without the suffix and skip
              // it. Otherwise, check if there also the .for_install variant
              // and if so, use that instead. While we could probably do this
              // more efficiently by remembering what we saw in vars, this is
              // not performance-sensitive and so we keep it simple for now.
              //
              const string* name;
              {
                const string& v (var->name);
                size_t n (v.size ());

                if (n > pfx.size () + 1 + 12 && // <prefix>..for_install
                    v.compare (n - 12, 12, ".for_install") == 0)
                {
                  tmp.assign (v, 0, n - 12);

                  if (t.vars.find (tmp) == t.vars.end ())
                    fail << v << " variant without " << tmp << " in library "
                         << l;

                  continue;
                }
                else
                {
                  name = &v;

                  tmp = v; tmp += ".for_install";

                  auto i (t.vars.find (tmp));
                  if (i != t.vars.end ())
                    var = &i->first.get ();
                }
              }

              if (dup)
              {
                if (find_if (vars.begin (), vars.end (),
                             [name] (const binding& p)
                             {
                               return *p.name == *name;
                             }) != vars.end ())
                  continue;
              }

              // Re-lookup the value in order to apply target type/pattern
              // specific prepends/appends.
              //
              lookup l (t[*var]);
              assert (l.defined ());

              vars.push_back (binding {name, var, l.value});
            }
          };

          append (g, false);

          if (!common)
          {
            if (l.group != nullptr)
              append (*l.group, true);
          }

          // First write the build2.metadata variable with the version,
          // prefix, and all the variable names/types (which should not
          // require any escaping).
          //
          os << endl
             << "build2.metadata = " << ver << ' ' << pfx;

          for (const binding& b: vars)
          {
            const variable& var (*b.var);
            const value& val (*b.val);

            // There is no notion of NULL in pkg-config variables and it's
            // probably best not to conflate them with empty.
            //
            if (val.null)
              fail << "null value in exported variable " << var
                   << " of library " << l;

            if (val.type == nullptr)
              fail << "untyped value in exported variable " << var
                   << " of library " << l;

            // Tighten this to only a sensible subset of types (see
            // parsing/serialization code for some of the potential problems).
            //
            if (!metadata_type (val.type->name).first)
              fail << "unsupported value type " << val.type->name
                   << " in exported variable " << var << " of library " << l;

            os << " \\" << endl
               << *b.name << '/' << val.type->name;
          }

          os << endl
             << endl;

          // Now the variables themselves.
          //
          string s; // Reuse the buffer.
          for (const binding& b: vars)
          {
            const variable& var (*b.var);
            const value& val (*b.val);

            names ns;
            names_view nv (reverse (val, ns, true /* reduce */));

            os << *b.name << " =";

            auto append = [&rel_base,
                           &reloc_path,
                           &reloc_dir_path,
                           &l, &var, &val, &s] (const name& v)
            {
              // If this is absolute path or dir_path, then attempt to
              // relocate. Without that the result will not be relocatable.
              //
              if (v.simple ())
              {
                path p;
                if (!rel_base.empty ()                                    &&
                    val.type != nullptr                                   &&
                    (val.type->is_a<path> () || val.type->is_a<paths> ()) &&
                    (p = path (v.value)).absolute ())
                {
                  p.normalize ();
                  s += reloc_path (p, var.name.c_str ());
                }
                else
                  s += v.value;
              }
              else if (v.directory ())
              {
                if (!rel_base.empty () && v.dir.absolute ())
                {
                  dir_path p (v.dir);
                  p.normalize ();
                  s += reloc_dir_path (p, var.name.c_str ());
                }
                else
                  s += v.dir.representation ();
              }
              else
                // It seems like we shouldn't end up here due to the type
                // check but let's keep it for good measure.
                //
                fail << "simple or directory value expected instead of '"
                     << v << "' in exported variable " << var << " of library "
                     << l;
            };

            for (auto i (nv.begin ()); i != nv.end (); ++i)
            {
              s.clear ();
              append (*i);

              if (i->pair)
              {
                // @@ What if the value contains the pair character? Maybe
                //    quote the halves in this case? Note: need to handle in
                //    parse_metadata() above if enable here. Note: none of the
                //    types currently allowed use pairs.
#if 0
                s += i->pair;
                append (*++i);
#else
                fail << "pair in exported variable " << var << " of library "
                     << l;
#endif
              }

              os << ' ' << escape (s);
            }

            os << endl;
          }
        }
        else
        {
          // No user metadata.
          //
          os << endl
             << "build2.metadata = 1" << endl;
        }

        // Save cc.type (see init() for the format documentation).
        //
        // Note that this value is set by link_rule and therefore should
        // be there.
        //
        {
          const string& t (
            cast<string> (
              l.state[a].lookup_original (
                c_type, true /* target_only */).first));

          // If common, then only save the language (the rest could be
          // static/shared-specific; strictly speaking even the language could
          // be, but that seems far fetched).
          //
          os << endl
             << "cc.type = " << (common ? string (t, 0, t.find (',')) : t)
             << endl;
        }

        // Save the bin.whole (whole archive) flag (see the link rule for
        // details on the lookup semantics).
        //
        if (la)
        {
          // Note: go straight for the public variable pool.
          //
          if (cast_false<bool> (l.lookup_original (
                                  ctx.var_pool["bin.whole"],
                                  true /* target_only */).first))
          {
            os << endl
               << "bin.whole = true" << endl;
          }
        }

        // If we have modules and/or importable headers, list them in the
        // respective variables. We also save some extra info about modules
        // (yes, the rabbit hole runs deep). This code is pretty similar to
        // compiler::search_modules().
        //
        // Note that we want to convey the importable headers information even
        // if modules are not enabled.
        //
        {
          struct module
          {
            string name;
            path file;

            string preprocessed;
            bool symexport;
          };
          vector<module> mods;

          // If we were to ever support another C-based language (e.g.,
          // Objective-C) and libraries that can use a mix of languages (e.g.,
          // C++ and Objective-C), then we would need to somehow reverse-
          // lookup header target type to language. Let's hope we don't.
          //
          vector<path>   x_hdrs;
          vector<path>   c_hdrs;

          // We need to (recursively) see through libu*{}. See similar logic
          // in search_modules().
          //
          // Note that the prerequisite targets are in the member, not the
          // group (for now we don't support different sets of modules/headers
          // for static/shared library; see load above for details).
          //
          auto collect = [a, this,
                          &mods,
                          &x_hdrs, &c_hdrs] (const target& l,
                                             const auto& collect) -> void
          {
            for (const target* pt: l.prerequisite_targets[a])
            {
              if (pt == nullptr)
                continue;

              if (modules && pt->is_a<bmix> ())
              {
                // What we have is a binary module interface. What we need is
                // a module interface source it was built from. We assume it's
                // the first mxx{} target that we see.
                //
                const target* mt (nullptr);
                for (const target* t: pt->prerequisite_targets[a])
                {
                  if (t != nullptr && (mt = t->is_a (*x_mod)))
                    break;
                }

                // Can/should there be a bmi{} without mxx{}? Can't think of a
                // reason.
                //
                assert (mt != nullptr);

                path p (install::resolve_file (mt->as<file> ()));

                if (p.empty ()) // Not installed.
                  continue;

                string pp;
                if (const string* v = cast_null<string> ((*mt)[x_preprocessed]))
                  pp = *v;

                mods.push_back (
                  module {
                    cast<string> (pt->state[a].vars[c_module_name]),
                    move (p),
                    move (pp),
                    symexport});
              }
              else if (pt->is_a (**this->x_hdrs) || pt->is_a<h> ())
              {
                if (cast_false<bool> ((*pt)[c_importable]))
                {
                  path p (install::resolve_file (pt->as<file> ()));

                  if (p.empty ()) // Not installed.
                    continue;

                  (pt->is_a<h> () ? c_hdrs : x_hdrs).push_back (move (p));
                }
              }
              // Note that in prerequisite targets we will have the libux{}
              // members, not the group.
              //
              else if (pt->is_a<libux> ())
                collect (*pt, collect);
            }
          };

          collect (l, collect);

          if (size_t n = mods.size ())
          {
            os << endl
               << "cxx.modules =";

            // The partition separator (`:`) is not a valid character in the
            // variable name. In fact, from the pkg-config source we can see
            // that the only valid special characters in variable names are
            // `_` and `.`. So to represent partition separators we use `..`,
            // for example hello.print..impl. While in the variable values we
            // can use `:`, for consistency we use `..` there as well.
            //
            for (module& m: mods)
            {
              size_t p (m.name.find (':'));
              if (p != string::npos)
                m.name.replace (p, 1, 2, '.');

              // Module names shouldn't require escaping.
              //
              os << (n != 1 ? " \\\n" : " ")
                 << m.name << '='
                 << escape (reloc_path (m.file, "module interface"));
            }

            os << endl;

            // Module-specific properties. The format is:
            //
            // <lang>.module_<property>.<module> = <value>
            //
            for (const module& m: mods)
            {
              if (!m.preprocessed.empty ())
                os << "cxx.module_preprocessed." << m.name << " = "
                   << m.preprocessed << endl;

              if (m.symexport)
                os << "cxx.module_symexport." << m.name << " = true" << endl;
            }
          }

          if (size_t n = c_hdrs.size ())
          {
            os << endl
               << "c.importable_headers =";

            for (const path& h: c_hdrs)
              os << (n != 1 ? " \\\n" : " ")
                 << escape (reloc_path (h, "header unit"));

            os << endl;
          }

          if (size_t n = x_hdrs.size ())
          {
            os << endl
               << x << ".importable_headers =";

            for (const path& h: x_hdrs)
              os << (n != 1 ? " \\\n" : " ")
                 << escape (reloc_path (h, "header unit"));

            os << endl;
          }
        }

        os.close ();
        arm.cancel ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << p << ": " << e;
      }
    }
  }
}
