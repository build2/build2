// file      : build2/cc/msvc.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/install/utility.hxx>

#include <build2/bin/target.hxx>
#include <build2/pkgconfig/target.hxx>

#include <build2/cc/types.hxx>
#include <build2/cc/utility.hxx>

#include <build2/cc/common.hxx>
#include <build2/cc/link.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // Try to find a .pc file in the pkgconfig/ subdirectory of libd, trying
    // several names derived from stem. If not found, return false. If found,
    // load poptions, loptions, and libs, set the corresponding *.export.*
    // variables on targets, and return true.
    //
    // System library search paths (those extracted from the compiler) are
    // passed in sys_sp and should already be extracted.
    //
    // Note that scope and link order should be "top-level" from the
    // search_library() POV.
    //
    bool common::
    pkgconfig_load (action act,
                    const scope& s,
                    lib& lt,
                    liba* at,
                    libs* st,
                    const optional<string>& proj,
                    const string& stem,
                    const dir_path& libd,
                    const dir_paths& sysd) const
    {
      tracer trace (x, "pkgconfig_load");

      assert (pkgconfig != nullptr);
      assert (at != nullptr || st != nullptr);

      // When it comes to looking for .pc files we have to decide where to
      // search (which directory(ies)) as well as what to search for (which
      // names). Suffix is our ".shared" or ".static" extension.
      //
      auto search_dir = [&proj, &stem, &libd] (const dir_path& dir,
                                               const string& sfx) -> path
      {
        // Check if we have this subdirectory in this library's directory.
        //
        dir_path pkgd (dir_path (libd) /= dir);

        if (!exists (pkgd))
          return path ();

        path f;

        // See if there is a corresponding .pc file. About half of them called
        // foo.pc and half libfoo.pc (and one of the pkg-config's authors
        // suggests that some of you should call yours foolib.pc, just to keep
        // things interesting, you know).
        //
        // Given the (general) import in the form <proj>%lib{<stem>}, we will
        // first try lib<stem>.pc, then <stem>.pc. Maybe it also makes sense
        // to try <proj>.pc, just in case. Though, according to pkg-config
        // docs, the .pc file should correspond to a library, not project. But
        // then you get something like zlib which calls it zlib.pc. So let's
        // just do it.
        //
        f = pkgd;
        f /= "lib";
        f += stem;
        f += sfx;
        f += ".pc";
        if (exists (f))
          return f;

        f = pkgd;
        f /= stem;
        f += sfx;
        f += ".pc";
        if (exists (f))
          return f;

        if (proj)
        {
          f = pkgd;
          f /= *proj;
          f += sfx;
          f += ".pc";
          if (exists (f))
            return f;
        }

        return path ();
      };

      auto search = [&search_dir, this] () -> pair<path, path>
      {
        pair<path, path> r;

        auto check = [&r, &search_dir] (const dir_path& d) -> bool
        {
          // First look for static/shared-specific files.
          //
          r.first  = search_dir (d, ".static");
          r.second = search_dir (d, ".shared");

          if (!r.first.empty () || !r.second.empty ())
            return true;

          // Then the common.
          //
          r.first = r.second = search_dir (d, string ());
          return !r.first.empty ();
        };

        // First always check the pkgconfig/ subdirectory in this library's
        // directory. Even on platforms where this is not the canonical place,
        // .pc files of autotools-based packages installed by the user often
        // still end up there.
        //
        if (!check (dir_path ("pkgconfig")))
        {
          // Platform-specific locations.
          //
          if (tsys == "freebsd")
          {
            // On FreeBSD .pc files go to libdata/pkgconfig/, not lib/pkgconfig/.
            //
            check ((dir_path ("..") /= "libdata") /= "pkgconfig");
          }
        }

        return r;
      };

      // To keep things simple, we run pkg-config multiple times, for
      // --cflag/--libs and --static.
      //
      auto extract = [this] (const path& f, const char* o, bool a) -> string
      {
        const char* args[] = {
          pkgconfig->recall_string (),
          o, // --cflags/--libs
          (a ? "--static" : f.string ().c_str ()),
          (a ? f.string ().c_str () : nullptr),
          nullptr
        };

        return run<string> (
          *pkgconfig, args, [] (string& s) -> string {return move (s);});
      };

      // On Windows pkg-config will escape backslahses in paths. In fact, it
      // may escape things even on non-Windows platforms, for example,
      // spaces. So we use a slightly modified version of next_word().
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

      // Extract --cflags and set them as lib?{}:export.poptions. Note that we
      // still pass --static in case this is pkgconf which has Cflags.private.
      //
      auto parse_cflags = [&trace, &extract, &next, this]
        (target& t, const path& f, bool a)
      {
        string cstr (extract (f, "--cflags", a));
        strings pops;

        string o;
        char arg ('\0');
        for (size_t b (0), e (0); !(o = next (cstr, b, e)).empty (); )
        {
          // Filter out /usr/local/include since most platforms/compilers
          // search in there (at the end, as in other system header
          // directories) by default. And for those that don't (like FreeBSD)
          // we should just fix it ourselves (see config_module::init ()).
          //
          // Failed that /usr/local/include may appear before "more specific"
          // directories which can lead to the installed headers being picked
          // up instead.
          //
          if (arg != '\0')
          {
            if (arg == 'I' && o == "/usr/local/include")
              pops.pop_back ();
            else
              pops.push_back (move (o));

            arg = '\0';
            continue;
          }

          size_t n (o.size ());

          // We only keep -I, -D and -U.
          //
          if (n >= 2 &&
              o[0] == '-' &&
              (o[1] == 'I' || o[1] == 'D' || o[1] == 'U'))
          {
            if (!(n > 2 &&
                  o[1] == 'I' &&
                  o.compare (2, string::npos, "/usr/local/include") == 0))
              pops.push_back (move (o));

            if (n == 2)
              arg = o[1];
            continue;
          }

          l4 ([&]{trace << "ignoring " << f << " --cflags option " << o;});
        }

        if (arg != '\0')
          fail << "argument expected after " << pops.back () <<
            info << "while parsing pkg-config --cflags output of " << f;

        if (!pops.empty ())
        {
          auto p (t.vars.insert (c_export_poptions));

          // The only way we could already have this value is if this same
          // library was also imported as a project (as opposed to installed).
          // Unlikely but possible. In this case the values were set by the
          // export stub and we shouldn't touch them.
          //
          if (p.second)
            p.first.get () = move (pops);
        }
      };

      // Parse --libs into loptions/libs (interface and implementation).
      //
      auto parse_libs = [act, &s, sysd, &extract, &next, this]
        (target& t, const path& f, bool a)
      {
        string lstr (extract (f, "--libs", a));

        strings lops;
        vector<name> libs;

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

            libs.push_back (name (move (o)));
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
          else if (tclass == "macos")
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

          prerequisite_key pk {
            nullopt, {&lib::static_type, &out, &out, &name, nullopt}, &s};

          if (lib* lt = static_cast<lib*> (
                search_library (act, sysd, usrd, pk)))
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

        // If all the -l's resolved and there were no other options, then drop
        // all the -L's. If we have unknown options, then leave them in to be
        // safe.
        //
        if (all && known)
          lops.clear ();

        if (!lops.empty ())
        {
          if (cid == compiler_id::msvc)
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

      pair<path, path> ps (search ());
      const path& ap (ps.first);
      const path& sp (ps.second);

      if (ap.empty () && sp.empty ())
        return false;

      if (at != nullptr && !ap.empty ())
      {
        parse_cflags (*at, ap, true);
        parse_libs (*at, ap, true);
      }

      if (st != nullptr && !sp.empty ())
      {
        parse_cflags (*st, sp, false);
        parse_libs (lt, sp, false); // Note: setting on lib{} (interface).
      }

      return true;
    }

    void link::
    pkgconfig_save (action act, const file& l, bool la) const
    {
      tracer trace (x, "pkgconfig_save");

      const scope& bs (l.base_scope ());
      const scope& rs (*bs.root_scope ());

      auto* pc (find_adhoc_member<pkgconfig::pc> (l));
      assert (pc != nullptr);

      const path& p (pc->path ());

      if (verb >= 2)
        text << "cat >" << p;

      try
      {
        ofdstream os (p);
        auto_rmfile arm (p);

        {
          const string& n (cast<string> (rs.vars[var_project]));

          lookup vl (rs.vars[var_version]);
          if (!vl)
            fail << "no version variable in project " << n <<
              info << "while generating " << p;

          const string& v (cast<string> (vl));

          os << "Name: " << n << endl;
          os << "Version: " << v << endl;

          // This one is required so make something up if unspecified.
          //
          os << "Description: ";
          if (const string* s = cast_null<string> (rs[var_project_summary]))
            os << *s << endl;
          else
            os << n << ' ' << v << endl;

          if (const string* u = cast_null<string> (rs[var_project_url]))
            os << "URL: " << *u << endl;
        }

        // In pkg-config backslashes, spaces, etc are escaped with a
        // backslash.
        //
        auto escape = [] (const string& s) -> string
        {
          string r;
          for (size_t p (0);;)
          {
            size_t sp (s.find_first_of ("\\ ", p));

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
        };

        auto save_poptions = [&l, &os, &escape] (const variable& var)
        {
          if (const strings* v = cast_null<strings> (l[var]))
          {
            for (auto i (v->begin ()); i != v->end (); ++i)
            {
              const string& o (*i);
              size_t n (o.size ());

              // Filter out -I (both -I<dir> and -I <dir> forms).
              //
              if (n >= 2 && o[0] == '-' && o[1] == 'I')
              {
                if (n == 2)
                  ++i;

                continue;
              }

              os << ' ' << escape (o);
            }
          }
        };

        // Given a library save its -l-style library name.
        //
        auto save_library = [&os] (const file& l)
        {
          // Use the .pc file name to derive the -l library name (in case of
          // the shared library, l.path() may contain version).
          //
          auto* pc (find_adhoc_member<pkgconfig::pc> (l));
          assert (pc != nullptr);

          // We also want to strip the lib prefix unless it is part of the
          // target name while keeping custom library prefix/suffix, if any.
          //
          string n (pc->path ().leaf ().base ().base ().string ());
          if (n.size () > 3 &&
              path::traits::compare (n.c_str (), 3, "lib", 3) == 0 &&
              path::traits::compare (n.c_str (), n.size (),
                                     l.name.c_str (), l.name.size ()) != 0)
          n.erase (0, 3);

          os << " -l" << n;
        };

        // By default we assume things go into install.{include, lib}.
        //
        // @@ TODO: support whole archive?
        //
        using install::resolve_dir;

        dir_path id (resolve_dir (l, cast<dir_path> (l["install.include"])));
        dir_path ld (resolve_dir (l, cast<dir_path> (l["install.lib"])));

        // Cflags.
        //
        os << "Cflags:";
        os << " -I" << escape (id.string ());
        save_poptions (c_export_poptions);
        save_poptions (x_export_poptions);
        os << endl;

        // Libs.
        //
        os << "Libs:";
        os << " -L" << escape (ld.string ());

        // Now process ourselves as if we were being linked to something (so
        // pretty similar to link::append_libraries()).
        //
        auto imp = [] (const file&, bool la) {return la;};

        auto lib = [&os, &save_library] (const file* l,
                                         const string& p,
                                         lflags,
                                         bool)
        {
          if (l != nullptr)
          {
            if (l->is_a<libs> () || l->is_a<liba> ()) // See through libux.
              save_library (*l);
          }
          else
            os << ' ' << p; // Something "system'y", pass as is.
        };

        auto opt = [&os] (const file&,
                          const string&,
                          bool, bool)
        {
          //@@ TODO: should we filter -L similar to -I?
          //@@ TODO: remember to use escape()

          /*
          // If we need an interface value, then use the group (lib{}).
          //
          if (const target* g = exp && l.is_a<libs> () ? l.group : &l)
          {
            const variable& var (
              com
              ? (exp ? c_export_loptions : c_loptions)
              : (t == x
                 ? (exp ? x_export_loptions : x_loptions)
                 : var_pool[t + (exp ? ".export.loptions" : ".loptions")]));

            append_options (args, *g, var);
          }
          */
        };

        process_libraries (
          act,
          bs,
          linfo {otype::e, la ? lorder::a_s : lorder::s_a}, // System-default.
          sys_lib_dirs,
          l, la,
          0, // Link flags.
          imp, lib, opt,
          true);

        os << endl;

        // If we have modules, list them in the modules variable. This code
        // is pretty similar to compiler::search_modules().
        //
        if (modules)
        {
          os << endl
             << "modules =";

          for (const target* pt: l.prerequisite_targets)
          {
            // @@ UTL: we need to (recursively) see through libux{} (and
            //    also in search_modules()).
            //
            if (pt != nullptr &&
                (pt->is_a<bmis> () ||
                 pt->is_a<bmia> () ||
                 pt->is_a<bmie> ()))
            {
              // What we have is a binary module interface. What we need is
              // a module interface source it was built from. We assume it's
              // the first mxx{} target that we see.
              //
              const target* mt (nullptr);
              for (const target* t: pt->prerequisite_targets)
              {
                if ((mt = t->is_a (*x_mod)))
                  break;
              }

              // Can/should there be a bmi{} without mxx{}? Can't think of a
              // reason.
              //
              assert (mt != nullptr);

              path p (install::resolve_file (mt->as<file> ()));

              if (p.empty ()) // Not installed.
                continue;

              const string& n (cast<string> (pt->vars[c_module_name]));

              // Module name shouldn't require escaping.
              //
              os << ' ' << n << '=' << escape (p.string ());
            }
          }

          os << endl;
        }

        os.close ();
        arm.cancel ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write " << p << ": " << e;
      }
    }
  }
}
