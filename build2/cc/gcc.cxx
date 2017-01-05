// file      : build2/cc/gcc.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/variable>
#include <build2/filesystem>
#include <build2/diagnostics>

#include <build2/bin/target>

#include <build2/cc/types>

#include <build2/cc/module>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // Extract system library search paths from GCC (gcc/g++) or compatible
    // (Clang, Intel) using the -print-search-dirs option.
    //
    dir_paths config_module::
    gcc_library_search_paths (process_path& xc, scope& rs) const
    {
      dir_paths r;

      cstrings args;
      string std; // Storage.

      args.push_back (xc.recall_string ());
      append_options (args, rs, c_coptions);
      append_options (args, rs, x_coptions);
      if (!tstd.empty ()) args.push_back (tstd.c_str ());
      append_options (args, rs, c_loptions);
      append_options (args, rs, x_loptions);
      args.push_back ("-print-search-dirs");
      args.push_back (nullptr);

      if (verb >= 3)
        print_process (args);

      string l;
      try
      {
        process pr (xc, args.data (), 0, -1); // Open pipe to stdout.

        try
        {
          ifdstream is (
            move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

          // The output of -print-search-dirs are a bunch of lines that start
          // with "<name>: =" where name can be "install", "programs", or
          // "libraries". If you have English locale, that is. If you set your
          // LC_ALL="tr_TR", then it becomes "kurulum", "programlar", and
          // "kitapl?klar". Also, Clang omits "install" while GCC and Intel
          // icc print all three. The "libraries" seem to be alwasy last,
          // however.
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

          if (!pr.wait ())
            throw failed ();
        }
        catch (const io_error&)
        {
          pr.wait ();
          fail << "error reading " << x_lang << " compiler -print-search-dirs "
               << "output";
        }
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }

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
        r.emplace_back (l, b, (e != string::npos ? e - b : e));
        r.back ().normalize ();

        if (e == string::npos)
          break;
      }

      return r;
    }
  }
}
