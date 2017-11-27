// file      : build2/cc/pkgconfig.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

// In order not to complicate the bootstrap procedure with libpkgconf building
// exclude functionality that involves reading of .pc files.
//
#ifndef BUILD2_BOOTSTRAP
#  include <libpkgconf/libpkgconf.h>
#endif

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/install/utility.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/types.hxx>
#include <build2/cc/target.hxx>  // pc
#include <build2/cc/utility.hxx>

#include <build2/cc/common.hxx>
#include <build2/cc/compile.hxx>
#include <build2/cc/link.hxx>

using namespace std;
using namespace butl;

namespace build2
{
#ifndef BUILD2_BOOTSTRAP
  // Load package information from a .pc file. Filter out the -I/-L options
  // that refer to system directories.
  //
  // Note that the prerequisite package .pc files search order is as follows:
  //
  // - in directory of the specified file
  // - in pc_dirs directories (in the natural order)
  //
  class pkgconf
  {
  public:
    using path_type = build2::path;

    path_type path;

  public:
    explicit
    pkgconf (path_type,
             const dir_paths& pc_dirs,
             const dir_paths& sys_inc_dirs,
             const dir_paths& sys_lib_dirs);

    // Create a special empty object. Querying package information on such
    // an object is illegal.
    //
    pkgconf () = default;

    ~pkgconf ();

    // Movable-only type.
    //
    pkgconf (pkgconf&& p)
        : path (move (p.path)),
          client_ (p.client_),
          pkg_ (p.pkg_)
    {
      p.client_ = nullptr;
      p.pkg_ = nullptr;
    }

    pkgconf&
    operator= (pkgconf&& p)
    {
      if (this != &p)
      {
        this->~pkgconf ();
        new (this) pkgconf (move (p)); // Assume noexcept move-construction.
      }
      return *this;
    }

    pkgconf (const pkgconf&) = delete;
    pkgconf& operator= (const pkgconf&) = delete;

    strings
    cflags (bool stat) const;

    strings
    libs (bool stat) const;

    string
    variable (const char*) const;

    string
    variable (const string& s) const {return variable (s.c_str ());}

  private:
    // Keep them as raw pointers not to deal with API thread-unsafety in
    // deleters and introducing additional mutex locks.
    //
    pkgconf_client_t* client_ = nullptr;
    pkgconf_pkg_t* pkg_ = nullptr;
  };

  // Currently the library is not thread-safe, even on the pkgconf_client_t
  // level (see issue #128 for details).
  //
  // @@ An update: seems that the obvious thread-safety issues are fixed.
  //    However, let's keep mutex locking for now not to introduce potential
  //    issues before we make sure there are no other ones.
  //
  static mutex pkgconf_mutex;

  // The package dependency traversal depth limit.
  //
  static const int pkgconf_max_depth = 100;

  // Normally the error_handler() callback can be called multiple times to
  // report a single error (once per message line), to produce a multi-line
  // message like this:
  //
  //   Package foo was not found in the pkg-config search path.\n
  //   Perhaps you should add the directory containing `foo.pc'\n
  //   to the PKG_CONFIG_PATH environment variable\n
  //   Package 'foo', required by 'bar', not found\n
  //
  // For the above example callback will be called 4 times. To suppress all the
  // junk we will use PKGCONF_PKG_PKGF_SIMPLIFY_ERRORS to get just:
  //
  //   Package 'foo', required by 'bar', not found\n
  //
  static const int pkgconf_flags = PKGCONF_PKG_PKGF_SIMPLIFY_ERRORS;

  static bool
  pkgconf_error_handler (const char* msg, const pkgconf_client_t*, const void*)
  {
    error << runtime_error (msg); // Sanitize the message.
    return true;
  }

  // Deleters. Note that they are thread-safe.
  //
  struct fragments_deleter
  {
    void operator() (pkgconf_list_t* f) const {pkgconf_fragment_free (f);}
  };

  // Convert fragments to strings. Skip the -I/-L options that refer to system
  // directories.
  //
  static strings
  to_strings (const pkgconf_list_t& frags,
              char type,
              const pkgconf_list_t& sysdirs)
  {
    assert (type == 'I' || type == 'L');

    strings r;

    auto add = [&r] (const pkgconf_fragment_t* frag)
    {
      string s;
      if (frag->type != '\0')
      {
        s += '-';
        s += frag->type;
      }

      s += frag->data;
      r.push_back (move (s));
    };

    // Option that is separated from its value, for example:
    //
    // -I /usr/lib
    //
    const pkgconf_fragment_t* opt (nullptr);

    pkgconf_node_t *node;
    PKGCONF_FOREACH_LIST_ENTRY(frags.head, node)
    {
      auto frag (static_cast<const pkgconf_fragment_t*> (node->data));

      // Add the separated option and directory, unless the latest is a system
      // one.
      //
      if (opt != nullptr)
      {
        // Note that we should restore the directory path that was
        // (mis)interpreted as an option, for example:
        //
        // -I -Ifoo
        //
        // In the above example option '-I' is followed by directory '-Ifoo',
        // which is represented by libpkgconf library as fragment 'foo' with
        // type 'I'.
        //
        if (!pkgconf_path_match_list (
              frag->type == '\0'
              ? frag->data
              : (string ({'-', frag->type}) + frag->data).c_str (),
              &sysdirs))
        {
          add (opt);
          add (frag);
        }

        opt = nullptr;
        continue;
      }

      // Skip the -I/-L option if it refers to a system directory.
      //
      if (frag->type == type)
      {
        // The option is separated from a value, that will (presumably) follow.
        //
        if (*frag->data == '\0')
        {
          opt = frag;
          continue;
        }

        if (pkgconf_path_match_list (frag->data, &sysdirs))
          continue;
      }

      add (frag);
    }

    if (opt != nullptr) // Add the dangling option.
      add (opt);

    return r;
  }

  // Note that some libpkgconf functions can potentially return NULL, failing
  // to allocate the required memory block. However, we will not check the
  // returned value for NULL as the library doesn't do so, prior to filling the
  // allocated structures. So such a code complication on our side would be
  // useless. Also, for some functions the NULL result has a special semantics,
  // for example "not found".
  //
  pkgconf::
  pkgconf (path_type p,
           const dir_paths& pc_dirs,
           const dir_paths& sys_lib_dirs,
           const dir_paths& sys_inc_dirs)
      : path (move (p))
  {
    auto add_dirs = [] (pkgconf_list_t& dir_list,
                        const dir_paths& dirs,
                        bool suppress_dups,
                        bool cleanup = false)
    {
      if (cleanup)
      {
        pkgconf_path_free (&dir_list);
        dir_list = PKGCONF_LIST_INITIALIZER;
      }

      for (const auto& d: dirs)
        pkgconf_path_add (d.string ().c_str (), &dir_list, suppress_dups);
    };

    mlock l (pkgconf_mutex);

    // Initialize the client handle.
    //
    unique_ptr<pkgconf_client_t, void (*) (pkgconf_client_t*)> c (
      pkgconf_client_new (pkgconf_error_handler,
                          nullptr /* error_handler_data */),
      [] (pkgconf_client_t* c) {pkgconf_client_free (c);});

    pkgconf_client_set_flags (c.get (), pkgconf_flags);

    // Note that the system header and library directory lists are
    // automatically pre-filled by the pkgconf_client_new() call (see above).
    // We will re-create these lists from scratch.
    //
    add_dirs (c->filter_libdirs,
              sys_lib_dirs,
              false /* suppress_dups */,
              true  /* cleanup */);

    add_dirs (c->filter_includedirs,
              sys_inc_dirs,
              false /* suppress_dups */,
              true  /* cleanup */);

    // Note that the loaded file directory is added to the (yet empty) search
    // list. Also note that loading of the prerequisite packages is delayed
    // until flags retrieval, and their file directories are not added to the
    // search list.
    //
    pkg_ = pkgconf_pkg_find (c.get (), path.string ().c_str ());

    if (pkg_ == nullptr)
      fail << "package '" << path << "' not found or invalid";

    // Add the .pc file search directories.
    //
    assert (c->dir_list.length == 1); // Package file directory (see above).
    add_dirs (c->dir_list, pc_dirs, true /* suppress_dups */);

    client_ = c.release ();
  }

  pkgconf::
  ~pkgconf ()
  {
    if (client_ != nullptr) // Not empty.
    {
      assert (pkg_ != nullptr);

      mlock l (pkgconf_mutex);
      pkgconf_pkg_unref (client_, pkg_);
      pkgconf_client_free (client_);
    }
  }

  strings pkgconf::
  cflags (bool stat) const
  {
    assert (client_ != nullptr); // Must not be empty.

    mlock l (pkgconf_mutex);

    pkgconf_client_set_flags (
      client_,
      pkgconf_flags |

      // Walk through the private package dependencies (Requires.private)
      // besides the public ones while collecting the flags. Note that we do
      // this for both static and shared linking.
      //
      PKGCONF_PKG_PKGF_SEARCH_PRIVATE |

      // Collect flags from Cflags.private besides those from Cflags for the
      // static linking.
      //
      (stat
       ? PKGCONF_PKG_PKGF_MERGE_PRIVATE_FRAGMENTS
       : 0));

    pkgconf_list_t f = PKGCONF_LIST_INITIALIZER; // Aggregate initialization.
    int e (pkgconf_pkg_cflags (client_, pkg_, &f, pkgconf_max_depth));

    if (e != PKGCONF_PKG_ERRF_OK)
      throw failed (); // Assume the diagnostics is issued.

    unique_ptr<pkgconf_list_t, fragments_deleter> fd (&f); // Auto-deleter.
    return to_strings (f, 'I', client_->filter_includedirs);
  }

  strings pkgconf::
  libs (bool stat) const
  {
    assert (client_ != nullptr); // Must not be empty.

    mlock l (pkgconf_mutex);

    pkgconf_client_set_flags (
      client_,
      pkgconf_flags |

      // Additionally collect flags from the private dependency packages
      // (see above) and from the Libs.private value for the static linking.
      //
      (stat
       ? PKGCONF_PKG_PKGF_SEARCH_PRIVATE |
         PKGCONF_PKG_PKGF_MERGE_PRIVATE_FRAGMENTS
       : 0));

    pkgconf_list_t f = PKGCONF_LIST_INITIALIZER; // Aggregate initialization.
    int e (pkgconf_pkg_libs (client_, pkg_, &f, pkgconf_max_depth));

    if (e != PKGCONF_PKG_ERRF_OK)
      throw failed (); // Assume the diagnostics is issued.

    unique_ptr<pkgconf_list_t, fragments_deleter> fd (&f); // Auto-deleter.
    return to_strings (f, 'L', client_->filter_libdirs);
  }

  string pkgconf::
  variable (const char* name) const
  {
    assert (client_ != nullptr); // Must not be empty.

    mlock l (pkgconf_mutex);
    const char* r (pkgconf_tuple_find (client_, &pkg_->vars, name));
    return r != nullptr ? string (r) : string ();
  }

#endif

  namespace cc
  {
    using namespace bin;

    // In pkg-config backslashes, spaces, etc are escaped with a backslash.
    //
    static string
    escape (const string& s)
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
    }

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
#ifndef BUILD2_BOOTSTRAP
    bool common::
    pkgconfig_load (action act,
                    const scope& s,
                    lib& lt,
                    liba* at,
                    libs* st,
                    const optional<string>& proj,
                    const string& stem,
                    const dir_path& libd,
                    const dir_paths& top_sysd,
                    const dir_paths& top_usrd) const
    {
      tracer trace (x, "pkgconfig_load");

      assert (at != nullptr || st != nullptr);

      // Iterate over pkgconf directories that correspond to the specified
      // library's directory, passing them to the callback function, until
      // the function returns false.
      //
      // First always check the pkgconfig/ subdirectory in this library's
      // directory. Even on platforms where this is not the canonical place,
      // .pc files of autotools-based packages installed by the user often
      // still end up there.
      //
      using callback = function<bool (dir_path&& d)>;
      auto pkgconf_dir = [this] (const dir_path& d, const callback& f) -> bool
      {
        dir_path pd (d);
        if (exists (pd /= "pkgconfig") && !f (move (pd)))
          return false;

        // Platform-specific locations.
        //
        if (tsys == "freebsd")
        {
          // On FreeBSD .pc files go to libdata/pkgconfig/, not lib/pkgconfig/.
          //
          pd = d;
          if (exists (((pd /= "..") /= "libdata") /= "pkgconfig") &&
              !f (move (pd)))
            return false;
        }

        return true;
      };

      // Same as above but iterate over pkgconfig directories for multiple
      // library directories.
      //
      auto pkgconf_dirs = [&pkgconf_dir] (const dir_paths& ds,
                                          const callback& f) -> bool
      {
        for (const auto& d: ds)
        {
          if (!pkgconf_dir (d, f))
            return false;
        }

        return true;
      };

      // When it comes to looking for .pc files we have to decide where to
      // search (which directory(ies)) as well as what to search for (which
      // names). Suffix is our ".shared" or ".static" extension.
      //
      auto search_dir = [&proj, &stem] (const dir_path& dir,
                                        const string& sfx) -> path
      {
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
        f = dir;
        f /= "lib";
        f += stem;
        f += sfx;
        f += ".pc";
        if (exists (f))
          return f;

        f = dir;
        f /= stem;
        f += sfx;
        f += ".pc";
        if (exists (f))
          return f;

        if (proj)
        {
          f = dir;
          f /= *proj;
          f += sfx;
          f += ".pc";
          if (exists (f))
            return f;
        }

        return path ();
      };

      auto search =
        [&libd, &search_dir, &pkgconf_dir, this] () -> pair<path, path>
      {
        pair<path, path> r;

        // Return false (and so stop to iterate) if .pc file is found.
        //
        // Note that we rely on "small function object" optimization here.
        //
        auto check = [&r, &search_dir] (dir_path&& d) -> bool
        {
          // First look for static/shared-specific files.
          //
          r.first  = search_dir (d, ".static");
          r.second = search_dir (d, ".shared");

          if (!r.first.empty () || !r.second.empty ())
            return false;

          // Then the common.
          //
          r.first = r.second = search_dir (d, string ());
          return r.first.empty ();
        };

        pkgconf_dir (libd, check);
        return r;
      };

      // Extract --cflags and set them as lib?{}:export.poptions. Note that we
      // still pass --static in case this is pkgconf which has Cflags.private.
      //
      auto parse_cflags = [&trace, this] (target& t, const pkgconf& pc, bool a)
      {
        strings pops;

        bool arg (false);
        for (auto& o: pc.cflags (a))
        {
          if (arg)
          {
            // Can only be an argument for -I, -D, -U options.
            //
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

          l4 ([&]{trace << "ignoring " << pc.path << " --cflags option "
                        << o;});
        }

        if (arg)
          fail << "argument expected after " << pops.back () <<
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
            p.first.get () = move (pops);
        }
      };

      // Parse --libs into loptions/libs (interface and implementation). If
      // ps is not NULL, add each resolves library target as a prerequisite.
      //
      auto parse_libs = [act, &s, top_sysd, this]
        (target& t, const pkgconf& pc, bool a, prerequisites* ps)
      {
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
        for (auto& o: pc.libs (a))
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

            // @@ If by some reason this is the library itself (doesn't go
            //    first or libpkgconf parsed libs in some bizarre way) we will
            //    hang trying to lock it's target inside search_library() (or
            //    fail an assertion if run serially) as by now it is already
            //    locked. To be safe we probably shouldn't rely on the position
            //    and filter out all occurrences of the library itself (by
            //    name?) and complain if none were encountered.
            //
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
            info << "while parsing pkg-config --libs " << pc.path;

        // Space-separated list of escaped library flags.
        //
        auto lflags = [&pc, a] () -> string
        {
          string r;
          for (const auto& o: pc.libs (a))
          {
            if (!r.empty ())
              r += ' ';
            r += escape (o);
          }
          return r;
        };

        if (first)
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
                  fail << "relative -L directory in '" << lflags () << "'" <<
                    info << "while parsing pkg-config --libs " << pc.path;

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

          if (const target* lt = search_library (act, top_sysd, usrd, pk))
          {
            // We used to pick a member but that doesn't seem right since the
            // same target could be used with different link orders.
            //
            n.dir = lt->dir;
            n.type = lib::static_type.name;
            n.value = lt->name;

            if (ps != nullptr)
              ps->push_back (prerequisite (*lt));
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
          if (cclass == compiler_class::msvc)
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

      // Parse modules and add them to the prerequisites.
      //
      auto parse_modules = [&trace, &next, this]
        (const pkgconf& pc, prerequisites& ps)
      {
        string mstr (pc.variable ("modules"));

        string m;
        for (size_t b (0), e (0); !(m = next (mstr, b, e)).empty (); )
        {
          // The format is <name>=<path>.
          //
          size_t p (m.find ('='));
          if (p == string::npos ||
              p == 0            || // Empty name.
              p == m.size () - 1)  // Empty path.
            fail << "invalid module information in '" << mstr << "'" <<
              info << "while parsing pkg-config --variable=modules "
                   << pc.path;

          string mn (m, 0, p);
          path mp (m, p + 1, string::npos);
          path mf (mp.leaf ());

          // Extract module properties, if any.
          //
          string pp (pc.variable ("module_preprocessed." + mn));
          string se (pc.variable ("module_symexport." + mn));

          // For now we assume these are C++ modules. There aren't any other
          // kind currently but if there were we would need to encode this
          // information somehow (e.g., cxx_modules vs c_modules variable
          // names).
          //
          auto tl (
            targets.insert_locked (
              *x_mod,
              mp.directory (),
              dir_path (),
              mf.base ().string (),
              mf.extension (),
              true, // Implied.
              trace));

          target& mt (tl.first);

          // If the target already exists, then setting its variables is not
          // MT-safe. So currently we only do it if we have the lock (and thus
          // nobody can see this target yet) assuming that this has already
          // been done otherwise.
          //
          // @@ This is not quite correct, though: this target could already
          //    exist but for a "different purpose" (e.g., it could be used as
          //    a header).
          //
          if (tl.second.owns_lock ())
          {
            mt.vars.assign (c_module_name) = move (mn);

            // Set module properties. Note that if unspecified we should still
            // set them to their default values since the hosting project may
            // have them set to incompatible value.
            //
            {
              value& v (mt.vars.assign (x_preprocessed)); // NULL
              if (!pp.empty ()) v = move (pp);
            }

            {
              mt.vars.assign (*x_symexport) = (se == "true");
            }

            tl.second.unlock ();
          }

          ps.push_back (prerequisite (mt));
        }
      };

      pair<path, path> pp (search ());
      const path& ap (pp.first);
      const path& sp (pp.second);

      if (ap.empty () && sp.empty ())
        return false;

      // For now we only populate prerequisites for lib{}. To do it for
      // liba{} would require weeding out duplicates that are already in
      // lib{}.
      //
      prerequisites prs;

      pkgconf apc;
      pkgconf spc;

      // Create the .pc files search directory list.
      //
      dir_paths pc_dirs;

      // Note that we rely on "small function object" optimization here.
      //
      auto add_pc_dir = [&pc_dirs] (dir_path&& d) -> bool
      {
        pc_dirs.emplace_back (move (d));
        return true;
      };

      pkgconf_dir  (libd,     add_pc_dir);
      pkgconf_dirs (top_usrd, add_pc_dir);
      pkgconf_dirs (top_sysd, add_pc_dir);

      // First sort out the interface dependencies (which we are setting on
      // lib{}). If we have the shared .pc variant, then we use that.
      // Otherwise -- static but extract without the --static option (see also
      // the saving logic).
      //
      pkgconf& ipc (sp.empty () ? apc : spc); // Interface package info.

      bool pa (at != nullptr && !ap.empty ());
      if (pa || sp.empty ())
        apc = pkgconf (ap, pc_dirs, sys_lib_dirs, sys_inc_dirs);

      bool ps (st != nullptr && !sp.empty ());
      if (ps || ap.empty ())
        spc = pkgconf (sp, pc_dirs, sys_lib_dirs, sys_inc_dirs);

      parse_libs (lt, ipc, false, &prs);

      if (pa)
      {
        parse_cflags (*at, apc, true);
        parse_libs (*at, apc, true, nullptr);
      }

      if (ps)
        parse_cflags (*st, spc, false);

      // For now we assume static and shared variants export the same set of
      // modules. While technically possible, having a different set will
      // most likely lead to all sorts of trouble (at least for installed
      // libraries) and life is short.
      //
      if (modules)
        parse_modules (ipc, prs);

      assert (!lt.has_prerequisites ());
      if (!prs.empty ())
        lt.prerequisites (move (prs));

      // Bless the library group with a "trust me it exists" timestamp. Failed
      // that, if we add it as a prerequisite (like we do above), the fallback
      // file rule won't match.
      //
      lt.mtime (file_mtime (ipc.path));

      return true;
    }

#else

    bool common::
    pkgconfig_load (action,
                    const scope&,
                    lib&,
                    liba*,
                    libs*,
                    const optional<string>&,
                    const string&,
                    const dir_path&,
                    const dir_paths&,
                    const dir_paths&) const
    {
      return false;
    }

#endif

    void link::
    pkgconfig_save (action act, const file& l, bool la) const
    {
      tracer trace (x, "pkgconfig_save");

      const scope& bs (l.base_scope ());
      const scope& rs (*bs.root_scope ());

      auto* t (find_adhoc_member<pc> (l));
      assert (t != nullptr);

      const path& p (t->path ());

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

        auto save_poptions = [&l, &os] (const variable& var)
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
        auto save_library = [&os, this] (const file& l)
        {
          // If available (it may not, in case of import-installed libraris),
          // use the .pc file name to derive the -l library name (in case of
          // the shared library, l.path() may contain version).
          //
          string n;

          auto strip_lib = [&n] ()
          {
            if (n.size () > 3 &&
                path::traits::compare (n.c_str (), 3, "lib", 3) == 0)
              n.erase (0, 3);
          };

          if (auto* t = find_adhoc_member<pc> (l))
          {
            // We also want to strip the lib prefix unless it is part of the
            // target name while keeping custom library prefix/suffix, if any.
            //
            n = t->path ().leaf ().base ().base ().string ();

            if (path::traits::compare (n.c_str (), n.size (),
                                       l.name.c_str (), l.name.size ()) != 0)
              strip_lib ();
          }
          else
          {
            // Derive -l-name from the file name in a fuzzy, platform-specific
            // manner.
            //
            n = l.path ().leaf ().base ().string ();

            if (cclass != compiler_class::msvc)
              strip_lib ();
          }

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
        // While we generate split shared/static .pc files, in case of static
        // we still want to sort things out into Libs/Libs.private. This is
        // necessary to distinguish between interface and implementation
        // dependencies if we don't have the shared variant (see the load
        // logic for details).
        //
        //@@ TODO: would be nice to weed out duplicates. But is it always
        //   safe? Think linking archives: will have to keep duplicates in
        //   the second position, not first. Gets even trickier with
        //   Libs.private split.
        //
        os << "Libs:";
        os << " -L" << escape (ld.string ());

        // Now process ourselves as if we were being linked to something (so
        // pretty similar to link::append_libraries()).
        //
        bool priv (false);
        auto imp = [&priv] (const file&, bool la) {return priv && la;};

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

        auto opt = [] (const file&,
                       const string&,
                       bool, bool)
        {
          //@@ TODO: should we filter -L similar to -I?
          //@@ TODO: how will the Libs/Libs.private work?
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

        // Pretend we are linking an executable using what would be normal,
        // system-default link order.
        //
        linfo li {otype::e, la ? lorder::a_s : lorder::s_a};

        process_libraries (act, bs, li, sys_lib_dirs,
                           l, la, 0, // Link flags.
                           imp, lib, opt, true);
        os << endl;

        if (la)
        {
          os << "Libs.private:";

          priv = true;
          process_libraries (act, bs, li, sys_lib_dirs,
                             l, la, 0, // Link flags.
                             imp, lib, opt, false);
          os << endl;
        }

        // If we have modules, list them in the modules variable. We also save
        // some extra info about them (yes, the rabbit hole runs deep). This
        // code is pretty similar to compiler::search_modules().
        //
        if (modules)
        {
          struct module
          {
            string name;
            path file;

            string pp;
            bool symexport;
          };
          vector<module> modules;

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

              string pp;
              if (const string* v = cast_null<string> ((*mt)[x_preprocessed]))
                pp = *v;

              modules.push_back (
                module {
                  cast<string> (pt->vars[c_module_name]),
                  move (p),
                  move (pp),
                  symexport
                });
            }
          }

          if (!modules.empty ())
          {
            os << endl
               << "modules =";

            // Module names shouldn't require escaping.
            //
            for (const module& m: modules)
              os << ' ' << m.name << '=' << escape (m.file.string ());

            os << endl;

            // Module-specific properties. The format is:
            //
            // module_<property>.<module> = <value>
            //
            for (const module& m: modules)
            {
              if (!m.pp.empty ())
                os << "module_preprocessed." << m.name << " = " << m.pp << endl;

              if (m.symexport)
                os << "module_symexport." << m.name << " = true" << endl;
            }
          }
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
