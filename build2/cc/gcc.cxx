// file      : build2/cc/gcc.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>

#include <build2/cc/types.hxx>

#include <build2/cc/module.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // Extract system header search paths from GCC (gcc/g++) or compatible
    // (Clang, Intel) using the -v -E </dev/null method.
    //
    dir_paths config_module::
    gcc_header_search_paths (const process_path& xc, scope& rs) const
    {
      dir_paths r;

      cstrings args;
      string std; // Storage.

      args.push_back (xc.recall_string ());
      append_options (args, rs, c_coptions);
      append_options (args, rs, x_coptions);
      append_options (args, tstd);

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

      if (verb >= 3)
        print_process (args);

      try
      {
        // Open pipe to stderr, redirect stdin and stdout to /dev/null.
        //
        process pr (xc, args.data (), -2, -2, -1);

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
          // is the presence of the "#include <...>" substring in the
          // "opening" line and the fact that the paths are indented with a
          // single space character, unlike the "closing" line.
          //
          // Note that on Mac OS we will also see some framework paths among
          // system header paths, followed with a comment. For example:
          //
          //  /Library/Frameworks (framework directory)
          //
          // For now we ignore framework paths and to filter them out we will
          // only consider valid paths to existing directories, skipping those
          // which we fail to normalize or stat.
          //
          string s;
          for (bool found (false); getline (is, s); )
          {
            if (!found)
              found = s.find ("#include <...>") != string::npos;
            else
            {
              if (s[0] != ' ')
                break;

              try
              {
                dir_path d (s, 1, s.size () - 1);

                if (d.absolute () && exists (d, true) &&
                    find (r.begin (), r.end (), d.normalize ()) == r.end ())
                  r.emplace_back (move (d));
              }
              catch (const invalid_path&) {}
            }
          }

          is.close (); // Don't block.

          if (!pr.wait ())
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
          pr.wait ();
          fail << "error reading " << x_lang << " compiler -v -E output";
        }
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child)
          exit (1);

        throw failed ();
      }

      // It's highly unlikely not to have any system directories. More likely
      // we misinterpreted the compiler output.
      //
      if (r.empty ())
        fail << "unable to extract " << x_lang << " compiler system header "
             << "search paths";

      return r;
    }

    // Extract system library search paths from GCC (gcc/g++) or compatible
    // (Clang, Intel) using the -print-search-dirs option.
    //
    dir_paths config_module::
    gcc_library_search_paths (const process_path& xc, scope& rs) const
    {
      dir_paths r;

      cstrings args;
      string std; // Storage.

      args.push_back (xc.recall_string ());
      append_options (args, rs, c_coptions);
      append_options (args, rs, x_coptions);
      append_options (args, tstd);
      append_options (args, rs, c_loptions);
      append_options (args, rs, x_loptions);
      args.push_back ("-print-search-dirs");
      args.push_back (nullptr);

      if (verb >= 3)
        print_process (args);

      // Open pipe to stdout.
      //
      process pr (run_start (xc,
                             args.data (),
                             0, /* stdin */
                             -1 /* stdout */));

      string l;
      try
      {
        ifdstream is (
          move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        // The output of -print-search-dirs are a bunch of lines that start
        // with "<name>: =" where name can be "install", "programs", or
        // "libraries". If you have English locale, that is. If you set your
        // LC_ALL="tr_TR", then it becomes "kurulum", "programlar", and
        // "kitapl?klar". Also, Clang omits "install" while GCC and Intel icc
        // print all three. The "libraries" seem to be alwasy last, however.
        //
        string s;
        for (bool found (false); !found && getline (is, s); )
        {
          found = (s.compare (0, 12, "libraries: =") == 0);

          size_t p (found ? 9 : s.find (": ="));

          if (p != string::npos)
            l.assign (s, p + 3, string::npos);
        }

        is.close (); // Don't block.
      }
      catch (const io_error&)
      {
        pr.wait ();
        fail << "error reading " << x_lang << " compiler -print-search-dirs "
             << "output";
      }

      run_finish (args, pr);

      if (l.empty ())
        fail << "unable to extract " << x_lang << " compiler system library "
             << "search paths";

      // Now the fun part: figuring out which delimiter is used. Normally it
      // is ':' but on Windows it is ';' (or can be; who knows for sure). Also
      // note that these paths are absolute (or should be). So here is what we
      // are going to do: first look for ';'. If found, then that's the
      // delimiter. If not found, then there are two cases: it is either a
      // single Windows path or the delimiter is ':'. To distinguish these two
      // cases we check if the path starts with a Windows drive.
      //
      char d (';');
      string::size_type e (l.find (d));

      if (e == string::npos &&
          (l.size () < 2 || l[0] == '/' || l[1] != ':'))
      {
        d = ':';
        e = l.find (d);
      }

      // Now chop it up. We already have the position of the first delimiter
      // (if any).
      //
      for (string::size_type b (0);; e = l.find (d, (b = e + 1)))
      {
        dir_path d (l, b, (e != string::npos ? e - b : e));

        if (find (r.begin (), r.end (), d.normalize ()) == r.end ())
          r.emplace_back (move (d));

        if (e == string::npos)
          break;
      }

      return r;
    }
  }
}
