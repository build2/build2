// file      : build/cxx/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/rule>

#include <cstddef>  // size_t
#include <cstdlib>  // exit
#include <string>
#include <vector>
#include <iostream>

#include <ext/stdio_filebuf.h>

#include <build/process>
#include <build/timestamp>

using namespace std;

namespace build
{
  namespace cxx
  {
    // compile
    //
    recipe compile::
    match (target& t) const
    {
      // @@ TODO:
      //
      // - check prerequisites: single source file
      // - check prerequisites: the rest are headers (issue warning at v=1?)
      // - if path already assigned, verify extension
      //
      // @@ Q:
      //
      // - if there is no .cxx, are we going to check if the one derived
      //   from target exist or can be built? If we do that, then it
      //   probably makes sense to try other rules first (two passes).
      //
      // - Wouldn't it make sense to cache source file? Careful: unloading
      //   of dependency info.
      //

      // See if we have a source file.
      //
      const cxx* s (nullptr);
      for (const target& p: t.prerequisites ())
      {
        if ((s = dynamic_cast<const cxx*> (&p)) != nullptr)
          break;
      }

      if (s == nullptr)
        return recipe ();

      // Derive object file name from target name.
      //
      obj& o (dynamic_cast<obj&> (t));

      if (o.path ().empty ())
        o.path (path (o.name () + ".o"));

      // Inject additional prerequisites.
      //
      // @@ If this failed, saying that the rule did not match is
      //    not quite correct.
      //
      if (!inject_prerequisites (o, *s))
        return recipe ();

      return recipe (&update);
    }

    // Return the next make prerequisite starting from the specified
    // position and update position to point to the start of the
    // following prerequisite or l.size() if there are none left.
    //
    static string
    next (const string& l, size_t& p)
    {
      size_t n (l.size ());

      // Skip leading spaces.
      //
      for (; p != n && l[p] == ' '; p++) ;

      // Lines containing multiple prerequisites are 80 characters max.
      //
      string r;
      r.reserve (n);

      // Scan the next prerequisite while watching out for escape sequences.
      //
      for (; p != n && l[p] != ' '; p++)
      {
        char c (l[p]);

        if (c == '\\')
          c = l[++p];

        r += c;
      }

      // Skip trailing spaces.
      //
      for (; p != n && l[p] == ' '; p++) ;

      // Skip final '\'.
      //
      if (p == n - 1 && l[p] == '\\')
        p++;

      return r;
    }

    bool compile::
    inject_prerequisites (obj& o, const cxx& s) const
    {
      const char* args[] = {
        "g++-4.9",
        "-std=c++11",
        "-I..",
        "-M",
        "-MG",      // Treat missing headers as generated.
        "-MQ", "*", // Quoted target (older version can't handle empty name).
        s.path ().string ().c_str (),
        nullptr};

      try
      {
        process pr (args, false, false, true);
        bool r (true);

        __gnu_cxx::stdio_filebuf<char> fb (pr.in_ofd, ios_base::in);
        istream is (&fb);

        for (bool first (true); !is.eof (); )
        {
          string l;
          getline (is, l);

          if (is.fail () && !is.eof ())
          {
            cerr << "warning: io error while parsing output" << endl;
            r = false;
            break;
          }

          size_t p (0);

          if (first)
          {
            // Empty output usually means the wait() call below will return
            // false.
            //
            if (l.empty ())
            {
              r = false;
              break;
            }

            first = false;
            assert (l[0] == '*' && l[1] == ':' && l[2] == ' ');
            next (l, (p = 3)); // Skip the source file.
          }

          while (p != l.size ())
          {
            path d (next (l, p));

            // If there is no extension (e.g., std C++ headers), then
            // assume it is a header. Otherwise, let the normall
            // mechanism to figure the type from the extension.
            //

            // @@ TODO:
            //
            // - memory leak

            hxx& h (*new hxx (d.leaf ().base ().string ()));
            h.path (d);

            o.prerequisite (h);
          }
        }

        //@@ Any diagnostics if wait() returns false. Or do we assume
        //   the child process issued something?
        //
        return pr.wait () && r;
      }
      catch (const process_error& e)
      {
        cerr << "warning: unable to execute '" << args[0] << "': " <<
          e.what () << endl;

        if (e.child ())
          exit (1);

        return false;
      }
    }

    target_state compile::
    update (target& t)
    {
      obj& o (dynamic_cast<obj&> (t));
      timestamp mt (o.mtime ());

      bool u (mt == timestamp_nonexistent);
      const cxx* s (nullptr);

      for (const target& p: t.prerequisites ())
      {
        // Assume all our prerequisites are mtime-based (checked in
        // match()).
        //
        if (!u)
        {
          const auto& mtp (dynamic_cast<const mtime_target&> (p));
          timestamp mp (mtp.mtime ());

          // What do we do if timestamps are equal? This can happen, for
          // example, on filesystems that don't have subsecond resolution.
          // There is not much we can do here except detect the case where
          // the prerequisite was updated in this run which means the
          // target must be out of date.
          //
          if (mt < mp || mt == mp && mtp.state () == target_state::updated)
            u = true;
        }

        if (s == nullptr)
          s = dynamic_cast<const cxx*> (&p);

        if (u && s != nullptr)
          break;
      }

      if (!u)
        return target_state::uptodate;

      const char* args[] = {
        "g++-4.9",
        "-std=c++11",
        "-I..",
        "-c",
        "-o", o.path ().string ().c_str (),
        s->path ().string ().c_str (),
        nullptr};

      cerr << "c++ " << *s << endl;

      try
      {
        process pr (args);

        if (!pr.wait ())
          return target_state::failed;

        // Should we go to the filesystem and get the new mtime? We
        // know the file has been modified, so instead just use the
        // current clock time. It has the advantage of having the
        // subseconds precision.
        //
        o.mtime (system_clock::now ());
        return target_state::updated;
      }
      catch (const process_error& e)
      {
        cerr << "error: unable to execute '" << args[0] << "': " <<
          e.what () << endl;

        if (e.child ())
          throw; // Let caller terminate us quickly without causing a scene.

        return target_state::failed;
      }
    }

    // link
    //
    recipe link::
    match (target& t) const
    {
      // @@ TODO:
      //
      // - check prerequisites: object files, libraries
      // - if path already assigned, verify extension
      //
      // @@ Q:
      //
      // - if there is no .o, are we going to check if the one derived
      //   from target exist or can be built? If we do that, then it
      //   probably makes sense to try other rules first (two passes).
      //   What if there is a library. Probably ok if .a, not is .so.
      //

      // See if we have at least one object file.
      //
      const obj* o (nullptr);
      for (const target& p: t.prerequisites ())
      {
        if ((o = dynamic_cast<const obj*> (&p)) != nullptr)
          break;
      }

      if (o == nullptr)
        return recipe ();

      // Derive executable file name from target name.
      //
      exe& e (dynamic_cast<exe&> (t));

      if (e.path ().empty ())
        e.path (path (e.name ()));

      return recipe (&update);
    }

    target_state link::
    update (target& t)
    {
      // @@ Q:
      //
      // - what are we doing with libraries?
      //

      exe& e (dynamic_cast<exe&> (t));
      timestamp mt (e.mtime ());

      bool u (mt == timestamp_nonexistent);

      for (const target& p: t.prerequisites ())
      {
        // Assume all our prerequisites are mtime-based (checked in
        // match()).
        //
        const auto& mtp (dynamic_cast<const mtime_target&> (p));
        timestamp mp (mtp.mtime ());

        // What do we do if timestamps are equal? This can happen, for
        // example, on filesystems that don't have subsecond resolution.
        // There is not much we can do here except detect the case where
        // the prerequisite was updated in this run which means the
        // target must be out of date.
        //
        if (mt < mp || mt == mp && mtp.state () == target_state::updated)
        {
          u = true;
          break;
        }
      }

      if (!u)
        return target_state::uptodate;

      vector<const char*> args {"g++-4.9", "-std=c++11", "-o"};

      args.push_back (e.path ().string ().c_str ());

      for (const target& p: t.prerequisites ())
      {
        const obj& o (dynamic_cast<const obj&> (p));
        args.push_back (o.path ().string ().c_str ());
      }

      args.push_back (nullptr);

      cerr << "ld " << e << endl;

      try
      {
        process pr (args.data ());

        if (!pr.wait ())
          return target_state::failed;

        // Should we go to the filesystem and get the new mtime? We
        // know the file has been modified, so instead just use the
        // current clock time. It has the advantage of having the
        // subseconds precision.
        //
        e.mtime (system_clock::now ());
        return target_state::updated;
      }
      catch (const process_error& e)
      {
        cerr << "error: unable to execute '" << args[0] << "': " <<
          e.what () << endl;

        if (e.child ())
          throw; // Let caller terminate us quickly without causing a scene.

        return target_state::failed;
      }
    }
  }
}
