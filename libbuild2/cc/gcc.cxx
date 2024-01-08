// file      : libbuild2/cc/gcc.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>

#include <libbuild2/cc/types.hxx>

#include <libbuild2/cc/module.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    void
    gcc_extract_library_search_dirs (const strings& v, dir_paths& r)
    {
      for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
      {
        const string& o (*i);

        dir_path d;
        try
        {
          // -L can either be in the "-L<dir>" or "-L <dir>" form.
          //
          if (o == "-L")
          {
            if (++i == e)
              break; // Let the compiler complain.

            d = dir_path (*i);
          }
          else if (o.compare (0, 2, "-L") == 0)
            d = dir_path (o, 2, string::npos);
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

#ifdef _WIN32
    // Some misconfigured MinGW GCC builds add absolute POSIX directories to
    // their built-in search paths (e.g., /mingw/{include,lib}) which GCC then
    // interprets as absolute paths relative to the current drive (so the set
    // of built-in search paths starts depending on where we run things from).
    //
    // While that's definitely misguided, life is short and we don't want to
    // waste it explaining this in long mailing list threads and telling
    // people to complain to whomever built their GCC. So we will just
    // recreate the behavior in a way that's consistent with GCC and let
    // people discover this on their own.
    //
    static inline void
    add_current_drive (string& s)
    {
      s.insert (0, work.string (), 0, 2); // Add e.g., `c:`.
    }
#endif

    // Parse color/semicolon-separated list of search directories (from
    // -print-search-dirs output, environment variables).
    //
    static void
    parse_search_dirs (const string& v, dir_paths& r,
                       const char* what, const char* what2 = "")
    {
      // Now the fun part: figuring out which delimiter is used. Normally it
      // is ':' but on Windows it is ';' (or can be; who knows for sure). Also
      // note that these paths are absolute (or should be). So here is what we
      // are going to do: first look for ';'. If found, then that's the
      // delimiter. If not found, then there are two cases: it is either a
      // single Windows path or the delimiter is ':'. To distinguish these two
      // cases we check if the path starts with a Windows drive.
      //
      char d (';');
      string::size_type e (v.find (d));

      if (e == string::npos &&
          (v.size () < 2 || v[0] == '/' || v[1] != ':'))
      {
        d = ':';
        e = v.find (d);
      }

      // Now chop it up. We already have the position of the first delimiter
      // (if any).
      //
      for (string::size_type b (0);; e = v.find (d, (b = e + 1)))
      {
        dir_path d;
        try
        {
          string ds (v, b, (e != string::npos ? e - b : e));

          // Skip empty entries (sometimes found in random MinGW toolchains).
          //
          if (!ds.empty ())
          {
#ifdef _WIN32
            if (path_traits::is_separator (ds[0]))
              add_current_drive (ds);
#endif
            d = dir_path (move (ds));

            if (d.relative ())
              throw invalid_path (move (d).string ());

            d.normalize ();
          }
        }
        catch (const invalid_path& e)
        {
          fail << "invalid directory '" << e.path << "'" << " in "
               << what << what2;
        }

        if (!d.empty () && find (r.begin (), r.end (), d) == r.end ())
          r.push_back (move (d));

        if (e == string::npos)
          break;
      }
    }

    // Extract system header search paths from GCC (gcc/g++) or compatible
    // (Clang, Intel) using the `-v -E </dev/null` method.
    //
    // Note that we currently do not return an accurate number of mode paths
    // though this information is currently not used for this compiler class.
    // It's not even clear whether we can do this correctly since GCC will
    // ignore an already-known system include path. Probably the only way to
    // do this is to run the compiler twice.
    //
    pair<dir_paths, size_t> config_module::
    gcc_header_search_dirs (const compiler_info& xi, scope& rs) const
    {
      dir_paths r;

      // Note also that any -I and similar that we may specify on the command
      // line are factored into the output. As well as the CPATH, etc.,
      // environment variable values.
      //
      cstrings args {xi.path.recall_string ()};
      append_options (args, rs, x_mode);

      // Compile as.
      //
      auto langopt = [this] () -> const char*
      {
        switch (x_lang)
        {
        case lang::c:   return "c";
        case lang::cxx: return "c++";
        }

        assert (false); // Can't get here.
        return nullptr;
      };

      args.push_back ("-x");
      args.push_back (langopt ());
      args.push_back ("-v");
      args.push_back ("-E");
      args.push_back ("-");
      args.push_back (nullptr);

      process_env env (xi.path);

      // For now let's assume that all the platforms other than Windows
      // recognize LC_ALL.
      //
#ifndef _WIN32
      const char* evars[] = {"LC_ALL=C", nullptr};
      env.vars = evars;
#endif

      if (verb >= 3)
        print_process (env, args);

      bool found_q (false); // Found `#include "..." ...` marker.
      bool found_b (false); // Found `#include <...> ...` marker.

      // Open pipe to stderr, redirect stdin and stdout to /dev/null.
      //
      process pr (run_start (
                    env,
                    args,
                    -2, /* stdin */
                    -2, /* stdout */
                    -1  /* stderr */));
      try
      {
        ifdstream is (
          move (pr.in_efd), fdstream_mode::skip, ifdstream::badbit);

        // Normally the system header paths appear between the following
        // lines:
        //
        // #include <...> search starts here:
        // End of search list.
        //
        // The exact text depends on the current locale. What we can rely on
        // is the presence of the "#include <...>" marker in the "opening"
        // line and the fact that the paths are indented with a single space
        // character, unlike the "closing" line.
        //
        // Note that on Mac OS we will also see some framework paths among
        // system header paths, followed with a comment. For example:
        //
        //  /Library/Frameworks (framework directory)
        //
        // For now we ignore framework paths and to filter them out we will
        // only consider valid paths to existing directories, skipping those
        // which we fail to normalize or stat. @@ Maybe this is a bit too
        // loose, especially compared to gcc_library_search_dirs()?
        //
        // Note that when there are no paths (e.g., because of -nostdinc),
        // then GCC prints both #include markers while Clang -- only "...".
        //
        for (string s; getline (is, s); )
        {
          if (!found_q)
            found_q = s.find ("#include \"...\"") != string::npos;
          else if (!found_b)
            found_b = s.find ("#include <...>") != string::npos;
          else
          {
            if (s[0] != ' ')
              break;

            dir_path d;
            try
            {
              string ds (s, 1, s.size () - 1);

#ifdef _WIN32
              if (path_traits::is_separator (ds[0]))
                add_current_drive (ds);
#endif
              d = dir_path (move (ds));

              if (d.relative () || !exists (d, true))
                continue;

              d.normalize ();
            }
            catch (const invalid_path&)
            {
              continue;
            }

            if (find (r.begin (), r.end (), d) == r.end ())
              r.emplace_back (move (d));
          }
        }

        is.close (); // Don't block.

        if (!run_wait (args, pr))
        {
          // We have read stderr so better print some diagnostics.
          //
          diag_record dr (fail);

          dr << "failed to extract " << x_lang << " header search paths" <<
            info << "command line: ";

          print_process (dr, args);
        }
      }
      catch (const io_error&)
      {
        run_wait (args, pr);
        fail << "error reading " << x_lang << " compiler -v -E output";
      }

      // Note that it's possible that we will have no system directories, for
      // example, if the user specified -nostdinc. But we must have still seen
      // at least one marker. Failed that we assume we misinterpreted the
      // compiler output.
      //
      if (!found_b && !found_q)
        fail << "unable to extract " << x_lang << " compiler system header "
             << "search paths";

      return make_pair (move (r), size_t (0));
    }

    // Extract system library search paths from GCC (gcc/g++) or compatible
    // (Clang, Intel) using the -print-search-dirs option.
    //
    pair<dir_paths, size_t> config_module::
    gcc_library_search_dirs (const compiler_info& xi, scope& rs) const
    {
      // The output of -print-search-dirs are a bunch of lines that start with
      // "<name>: =" where name can be "install", "programs", or "libraries".
      //
      // If you have English locale, that is. If you set your LC_ALL="tr_TR",
      // then it becomes "kurulum", "programlar", and "kitapl?klar". Also,
      // Clang omits "install" while GCC and Intel icc print all three. The
      // "libraries" seem to be always last, however. Also, the colon and
      // the following space in "<name>: =" can all be translated (e.g.,
      // in zh_CN.UTF-8).
      //
      // Maybe it's time we stop playing these games and start running
      // everything with LC_ALL=C? One drawback of this approach is that the
      // command that we print isn't exactly how we run. Maybe print it with
      // the environment variables in front? Also there is MinGW GCC.
      //
      // Note also that any -L that we may specify on the command line are not
      // factored into the output (unlike for headers above).
      //
      dir_paths r;

      // Extract -L paths from the compiler mode.
      //
      gcc_extract_library_search_dirs (cast<strings> (rs[x_mode]), r);
      size_t rn (r.size ());

      cstrings args {xi.path.recall_string ()};
      append_options (args, rs, x_mode);
      args.push_back ("-print-search-dirs");
      args.push_back (nullptr);

      process_env env (xi.path);

      // For now let's assume that all the platforms other than Windows
      // recognize LC_ALL.
      //
#ifndef _WIN32
      const char* evars[] = {"LC_ALL=C", nullptr};
      env.vars = evars;
#endif

      if (verb >= 3)
        print_process (env, args);

      // Open pipe to stdout.
      //
      // Note: this function is called in the serial load phase and so no
      // diagnostics buffering is needed.
      //
      process pr (run_start (env,
                             args,
                             0, /* stdin */
                             -1 /* stdout */));

      string l;
      try
      {
        ifdstream is (
          move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        string s;
        for (bool found (false); !found && getline (is, s); )
        {
          found = (s.compare (0, 12, "libraries: =") == 0);

          size_t p (found ? 9 : s.find ('='));

          if (p != string::npos)
            l.assign (s, p + 3, string::npos);
        }

        is.close (); // Don't block.
      }
      catch (const io_error& e)
      {
        if (run_wait (args, pr))
          fail << "io error reading " << args[0] << " -print-search-dirs "
               << "output: " << e;

        // If the child process has failed then assume the io error was caused
        // by that and let run_finish() deal with it.
      }

      run_finish (args, pr, 2 /* verbosity */);

      if (l.empty ())
        fail << "unable to extract " << x_lang << " compiler system library "
             << "search paths";

      parse_search_dirs (l, r, args[0], " -print-search-dirs output");

      // While GCC incorporates the LIBRARY_PATH environment variable value
      // into the -print-search-dirs output, Clang does not. Also, unlike GCC,
      // it appears to consider such paths last.
      //
      if (xi.id.type == compiler_type::clang)
      {
        if (optional<string> v = getenv ("LIBRARY_PATH"))
          parse_search_dirs (*v, r, "LIBRARY_PATH environment variable");
      }

      return make_pair (move (r), rn);
    }
  }
}
