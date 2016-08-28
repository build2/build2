// file      : build2/cc/msvc.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/variable>
#include <build2/filesystem>
#include <build2/diagnostics>

#include <build2/bin/target>

#include <build2/cc/types>
#include <build2/cc/utility>

#include <build2/cc/common>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // Try to find a .pc file in the pkgconfig/ subdirectory of libd, trying
    // several names derived from stem. If not found, return false. If found,
    // extract poptions, loptions, and libs, set the corresponding *.export.*
    // variables on targets, and return true.
    //
    // System library search paths (those extracted from the compiler) are
    // passed in sys_sp and should already be extracted.
    //
    // Note that scope and link order should be "top-level" from the
    // search_library() POV.
    //
    bool common::
    pkgconfig_extract (scope& s,
                       lib& lt,
                       liba* at,
                       libs* st,
                       const string* proj,
                       const string& stem,
                       const dir_path& libd,
                       const dir_paths& sysd) const
    {
      tracer trace (x, "pkgconfig_extract");

      assert (pkgconfig != nullptr);
      assert (at != nullptr || st != nullptr);

      // Check if we have the pkgconfig/ subdirectory in this library's
      // directory.
      //
      dir_path pkgd (dir_path (libd) /= "pkgconfig");

      if (!dir_exists (pkgd))
        return false;

      // Now see if there is a corresponding .pc file. About half of them
      // called foo.pc and half libfoo.pc (and one of the pkg-config's authors
      // suggests that some of you should call yours foolib.pc, just to keep
      // things interesting, you know).
      //
      // Given the (general) import in the form <proj>%lib{<stem>}, we will
      // first try <stem>.pc, then lib<stem>.pc. Maybe it also makes sense to
      // try <proj>.pc, just in case. Though, according to pkg-config docs,
      // the .pc file should correspond to a library, not project. But then
      // you get something like zlib which calls it zlib.pc. So let's just do
      // it.
      //
      path f;
      f = pkgd;
      f /= stem;
      f += ".pc";

      if (!file_exists (f))
      {
        f = pkgd;
        f /= "lib";
        f += stem;
        f += ".pc";

        if (!file_exists (f))
        {
          if (proj != nullptr)
          {
            f = pkgd;
            f /= *proj;
            f += ".pc";

            if (!file_exists (f))
              return false;
          }
          else
            return false;
        }
      }

      // Ok, we are in business. Time to run pkg-config. To keep things
      // simple, we run it multiple times, for --cflag/--libs and --static.
      //
      auto extract = [&f, this] (const char* op, bool impl) -> string
      {
        const char* args[] = {
          pkgconfig->initial,
          op, // --cflags/--libs
          (impl ? "--static" : f.string ().c_str ()),
          (impl ? f.string ().c_str () : nullptr),
          nullptr
        };

        return run<string> (
          *pkgconfig, args, [] (string& s) -> string {return move (s);});
      };

      // On Windows pkg-config (at least the MSYS2 one which we are using)
      // will escape backslahses in paths. In fact, it may escape things
      // even on non-Windows platforms, for example, spaces. So we use a
      // slightly modified version of next_word().
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

      // First extract --cflags and set them as lib{}:export.poptions (i.e.,
      // they will be common for both liba{} and libs{}; later when we do
      // split .pc files, we will have to run this twice).
      //
      {
        string cstr (extract ("--cflags", false));
        strings pops;

        bool arg (false);
        string o;
        for (size_t b (0), e (0); !(o = next (cstr, b, e)).empty (); )
        {
          if (arg)
          {
            pops.push_back (move (o));
            arg = false;
            continue;
          }

          size_t n (o.size ());

          // We only keep -I, -D and -U.
          //
          if (n >= 2 &&
              o[0] == '-' &&
              (o[1] == 'I' || o[1] == 'D' || o[1] == 'U'))
          {
            pops.push_back (move (o));
            arg = (n == 2);
            continue;
          }

          l4 ([&]{trace << "ignoring " << f << " --cflags option " << o;});
        }

        if (arg)
          fail << "argument expected after " << pops.back () <<
            info << "while parsing pkg-config --cflags output of " << f;

        if (!pops.empty ())
        {
          auto p (lt.vars.insert (c_export_poptions));

          // The only way we could already have this value is if this same
          // library was also imported as a project (as opposed to installed).
          // Unlikely but possible. In this case the values were set by the
          // export stub and we shouldn't touch them.
          //
          if (p.second)
            p.first.get () = move (pops);
        }
      }

      // Now parse --libs into loptions/libs (interface and implementation).
      //
      auto parse_libs = [&s, &f, sysd, &next, this] (
        const string& lstr, target& t)
      {
        strings lops;
        names libs;

        // Normally we will have zero or more -L's followed by one or more
        // -l's, with the first one being the library itself. But sometimes
        // we may have other linker options, for example, -Wl,... or
        // -pthread. It's probably a bad idea to ignore them. Also,
        // theoretically, we could have just the library name/path.
        //
        // The tricky part, of course, is to know whether what follows after
        // an option we don't recognize is its argument or another option or
        // library. What we do at the moment is stop recognizing just
        // library names (without -l) after seeing an unknown option.
        //

        bool arg (false), first (true), known (true), have_L;
        string o;
        for (size_t b (0), e (0); !(o = next (lstr, b, e)).empty (); )
        {
          if (arg)
          {
            // Can only be an argument for an loption.
            //
            lops.push_back (move (o));
            arg = false;
            continue;
          }

          size_t n (o.size ());

          // See if this is -L.
          //
          if (n >= 2 && o[0] == '-' && o[1] == 'L')
          {
            have_L = true;
            lops.push_back (move (o));
            arg = (n == 2);
            continue;
          }

          // See if that's -l or just the library name/path.
          //
          if ((known && o[0] != '-') ||
              (n > 2 && o[0] == '-' && o[1] == 'l'))
          {
            // First one is the library itself, which we skip. Note that we
            // don't verify this and theoretically it could be some other
            // library, but we haven't encountered such a beast yet.
            //
            if (first)
            {
              first = false;
              continue;
            }

            libs.push_back (name (move (o), false));
            continue;
          }

          // Otherwise we assume it is some other loption.
          //
          known = false;
          lops.push_back (move (o));
        }

        if (arg)
          fail << "argument expected after " << lops.back () <<
            info << "while parsing pkg-config --libs output of " << f;

        if (first)
          fail << "library expected in '" << lstr << "'" <<
            info << "while parsing pkg-config --libs output of " << f;

        // Resolve -lfoo into the library file path using our import installed
        // machinery (i.e., we are going to call search_library() that will
        // probably call us again, and so on).
        //
        // The reason we do it is the link order. For general libraries it
        // shouldn't matter if we imported them via an export stub, direct
        // import installed, or via a .pc file (which we could have generated
        // from the export stub). The exception is "runtime libraries" (which
        // are really the extension of libc) such as -lm, -ldl, -lpthread,
        // etc. Those we will detect and leave as -l*.
        //
        // If we managed to resolve all the -l's (sans runtime), then we can
        // omit -L's for nice and tidy command line.
        //
        bool all (true);
        optional<dir_paths> usrd; // Populate lazily.

        for (name& n: libs)
        {
          string& l (n.value);

          // These ones are common/standard/POSIX.
          //
          if (l[0] != '-'      || // e.g., shell32.lib
              l == "-lm"       ||
              l == "-ldl"      ||
              l == "-lrt"      ||
              l == "-lpthread")
            continue;

          // Note: these list are most likely incomplete.
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
          else if (tclass == "macosx")
          {
            if (l == "-lSystem")
              continue;
          }

          // Prepare user search paths by entering the -L paths from the .pc
          // file.
          //
          if (have_L && !usrd)
          {
            usrd = dir_paths ();

            for (auto i (lops.begin ()); i != lops.end (); ++i)
            {
              const string& o (*i);

              if (o.size () >= 2 && o[0] == '-' && o[1] == 'L')
              {
                string p;

                if (o.size () == 2)
                  p = *++i; // We've verified it's there.
                else
                  p = string (o, 2);

                dir_path d (move (p));

                if (d.relative ())
                  fail << "relative -L directory in '" << lstr << "'" <<
                    info << "while parsing pkg-config --libs output of " << f;

                usrd->push_back (move (d));
              }
            }
          }

          // @@ OUT: for now we assume out is undetermined, just like in
          // resolve_library().
          //
          dir_path out;
          string name (l, 2); // Sans -l.
          const string* ext (nullptr);

          prerequisite_key pk {
            nullptr, {&lib::static_type, &out, &out, &name, ext}, &s};

          if (lib* lt = static_cast<lib*> (search_library (sysd, usrd, pk)))
          {
            // We used to pick a member but that doesn't seem right since the
            // same target could be used with different link orders.
            //
            n.dir = lt->dir;
            n.type = lib::static_type.name;
            n.value = lt->name;
          }
          else
            // If we couldn't find the library, then leave it as -l.
            //
            all = false;
        }

        // If all the -l's resolved and no other options, then drop all the
        // -L's. If we have unknown options, then leave them in to be safe.
        //
        if (all && known)
          lops.clear ();

        if (lops.empty ())
        {
          if (cid == "msvc")
          {
            // Translate -L to /LIBPATH.
            //
            for (auto i (lops.begin ()); i != lops.end (); )
            {
              string& o (*i);
              size_t n (o.size ());

              if (n >= 2 && o[0] == '-' && o[1] == 'L')
              {
                o.replace (0, 2, "/LIBPATH:");

                if (n == 2)
                {
                  o += *++i; // We've verified it's there.
                  i = lops.erase (i);
                  continue;
                }
              }

              ++i;
            }
          }

          auto p (t.vars.insert (c_export_loptions));

          if (p.second)
            p.first.get () = move (lops);
        }

        // Set even if empty (export override).
        //
        {
          auto p (t.vars.insert (c_export_libs));

          if (p.second)
            p.first.get () = move (libs);
        }
      };

      {
        string lstr_int (extract ("--libs", false));
        string lstr_imp (extract ("--libs", true));

        parse_libs (lstr_int, lt);

        // Currently, these will result in the same values but it will be
        // different once we support split .pc files.
        //
        if (at != nullptr) parse_libs (lstr_imp, *at);
        if (st != nullptr) parse_libs (lstr_imp, *st);
      }

      return true;
    }
  }
}
