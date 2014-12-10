// file      : build/cxx/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/rule>

#include <vector>
#include <iostream>

#include <build/native>
#include <build/process>
#include <build/timestamp>

#include <build/cxx/target>

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

      return recipe (&update);
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
