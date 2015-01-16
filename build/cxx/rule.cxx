// file      : build/cxx/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/rule>

#include <string>
#include <vector>
#include <cstddef>  // size_t
#include <cstdlib>  // exit
#include <utility>  // move()
#include <iostream>

#include <ext/stdio_filebuf.h>

#include <build/scope>
#include <build/algorithm>
#include <build/process>
#include <build/timestamp>
#include <build/diagnostics>
#include <build/context>

using namespace std;

namespace build
{
  namespace cxx
  {
    // compile
    //
    void* compile::
    match (target& t, const string&) const
    {
      tracer tr ("cxx::compile::match");

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

      // See if we have a C++ source file.
      //
      for (prerequisite& p: t.prerequisites)
      {
        if (p.type.id == typeid (cxx))
          return &p;
      }

      trace (3, [&]{tr << "no c++ source file for target " << t;});
      return nullptr;
    }

    recipe compile::
    select (target& t, void* v) const
    {
      // Derive object file name from target name.
      //
      obj& o (dynamic_cast<obj&> (t));

      if (o.path ().empty ())
        o.path (o.directory / path (o.name + ".o"));

      // Resolve prerequisite to target and match it to a rule. We need
      // this in order to get the source file path for prerequisite
      // injections.
      //
      prerequisite* sp (static_cast<prerequisite*> (v));
      cxx* st (
        dynamic_cast<cxx*> (
          sp->target != nullptr ? sp->target : &search (*sp)));

      if (st != nullptr)
      {
        if (st->recipe () || build::match (*st))
        {
          // Don't bother if the file does not exist.
          //
          if (st->mtime () != timestamp_nonexistent)
            inject_prerequisites (o, *st, sp->scope);
        }
      }

      return &update;
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

    void compile::
    inject_prerequisites (obj& o, const cxx& s, scope& ds) const
    {
      tracer tr ("cxx::compile::inject_prerequisites");

      // We are using absolute source file path in order to get
      // absolute paths in the result.
      //
      const char* args[] = {
        "g++-4.9",
        "-std=c++14",
        "-I", src_root.string ().c_str (),
        "-MM",       //@@ -M
        "-MG",      // Treat missing headers as generated.
        "-MQ", "*", // Quoted target (older version can't handle empty name).
        s.path ().string ().c_str (),
        nullptr};

      if (verb >= 2)
        print_process (args);

      if (verb >= 5)
        tr << "target: " << o;

      try
      {
        process pr (args, false, false, true);

        __gnu_cxx::stdio_filebuf<char> fb (pr.in_ofd, ios_base::in);
        istream is (&fb);

        for (bool first (true); !is.eof (); )
        {
          string l;
          getline (is, l);

          if (is.fail () && !is.eof ())
          {
            cerr << "error: io error while parsing g++ -M output" << endl;
            throw error ();
          }

          size_t pos (0);

          if (first)
          {
            // Empty output should mean the wait() call below will return
            // false.
            //
            if (l.empty ())
              break;

            assert (l[0] == '*' && l[1] == ':' && l[2] == ' ');
            next (l, (pos = 3)); // Skip the source file.

            first = false;
          }

          while (pos != l.size ())
          {
            path file (next (l, pos));
            file.normalize ();

            if (verb >= 5)
              tr << "prerequisite path: " << file.string ();

            // If there is no extension (e.g., standard C++ headers),
            // then assume it is a header. Otherwise, let the standard
            // mechanism derive the type from the extension.
            //

            // @@ TODO:
            //

            // Split the name into its directory part, the name part, and
            // extension. Here we can assume the name part is a valid
            // filesystem name.
            //
            path d (file.directory ());
            string n (file.leaf ().base ().string ());
            const char* es (file.extension ());
            const string* e (&extension_pool.find (es != nullptr ? es : ""));

            // Find or insert.
            //
            prerequisite& p (
              ds.prerequisites.insert (
                hxx::static_type, move (d), move (n), e, ds, tr).first);

            // Resolve to target so that we can assign its path.
            //
            path_target& t (
              dynamic_cast<path_target&> (
                p.target != nullptr ? *p.target : search (p)));

            if (t.path ().empty ())
              t.path (move (file));

            o.prerequisites.push_back (p);
          }
        }

        // We assume the child process issued some diagnostics.
        //
        if (!pr.wait ())
          throw error ();
      }
      catch (const process_error& e)
      {
        cerr << "error: unable to execute '" << args[0] << "': " <<
          e.what () << endl;

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child ())
          exit (1);

        throw error ();
      }
    }

    target_state compile::
    update (target& t)
    {
      obj& o (dynamic_cast<obj&> (t));
      timestamp mt (o.mtime ());

      bool u (mt == timestamp_nonexistent);
      const cxx* s (nullptr);

      for (const prerequisite& p: t.prerequisites)
      {
        const target& pt (*p.target);

        // Assume all our prerequisites are mtime-based (checked in
        // match()).
        //
        if (!u)
        {
          const auto& mtp (dynamic_cast<const mtime_target&> (pt));
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
          s = dynamic_cast<const cxx*> (&pt);

        if (u && s != nullptr)
          break;
      }

      if (!u)
        return target_state::uptodate;

      // Translate paths to relative (to working directory) ones. This
      // results in easier to read diagnostics.
      //
      path ro (translate (o.path ()));
      path rs (translate (s->path ()));

      const char* args[] = {
        "g++-4.9",
        "-std=c++14",
        "-g",
        "-I", src_root.string ().c_str (),
        "-c",
        "-o", ro.string ().c_str (),
        rs.string ().c_str (),
        nullptr};

      if (verb >= 1)
        print_process (args);
      else
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

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child ())
          exit (1);

        return target_state::failed;
      }
    }

    // link
    //
    void* link::
    match (target& t, const string& hint) const
    {
      tracer tr ("cxx::link::match");

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
      //   What if there is a library. Probably ok if .a, not if .so.
      //

      // Scan prerequisites and see if we can work with what we've got.
      //
      bool seen_cxx (false), seen_c (false), seen_obj (false);

      for (prerequisite& p: t.prerequisites)
      {
        if (p.type.id == typeid (cxx))
        {
          if (!seen_cxx)
            seen_cxx = true;
        }
        else if (p.type.id == typeid (c))
        {
          if (!seen_c)
            seen_c = true;
        }
        else if (p.type.id == typeid (obj))
        {
          if (!seen_obj)
            seen_obj = true;
        }
        else
        {
          trace (3, [&]{tr << "unexpected prerequisite type " << p.type;});
          return nullptr;
        }
      }

      // We will only chain C source if there is also C++ source or we
      // we explicitly asked to.
      //
      if (seen_c && !seen_cxx && hint < "cxx")
      {
        trace (3, [&]{tr << "c prerequisite(s) without c++ or hint";});
        return nullptr;
      }

      return seen_cxx || seen_c || seen_obj ? &t : nullptr;
    }

    recipe link::
    select (target& t, void*) const
    {
      tracer tr ("cxx::link::select");

      // Derive executable file name from target name.
      //
      exe& e (dynamic_cast<exe&> (t));

      if (e.path ().empty ())
        e.path (e.directory / path (e.name));

      // Do rule chaining for C and C++ source files.
      //
      // @@ OPT: match() could indicate whether this is necesssary.
      //
      for (auto& pr: t.prerequisites)
      {
        prerequisite& cp (pr);

        if (cp.type.id != typeid (c) && cp.type.id != typeid (cxx))
          continue;

        // Come up with the obj{} prerequisite. The c(xx){} prerequisite
        // directory can be relative (to the scope) or absolute. If it is
        // relative, then we use it as is. If it is absolute, then translate
        // it to the corresponding directory under out_root. While the
        // c(xx){} directory is most likely under src_root, it is also
        // possible it is under out_root (e.g., generated source).
        //
        path d;
        if (cp.directory.relative () || cp.directory.sub (out_root))
          d = cp.directory;
        else
        {
          if (!cp.directory.sub (src_root))
          {
            cerr << "error: out of project prerequisite " << cp << endl;
            cerr << "info: specify corresponding obj{} target explicitly"
                 << endl;
            throw error ();
          }

          d = out_root / cp.directory.leaf (src_root);
        }

        prerequisite& op (
          cp.scope.prerequisites.insert (
            obj::static_type, move (d), cp.name, nullptr, cp.scope, tr).first);

        // Resolve this prerequisite to target.
        //
        target& ot (search (op));

        // If this target already exists, then it needs to be "compatible"
        // with what we doing.
        //
        bool add (true);
        for (prerequisite& p: ot.prerequisites)
        {
          // Ignore some known target types (headers).
          //
          if (p.type.id == typeid (h) ||
              cp.type.id == typeid (cxx) && (p.type.id == typeid (hxx) ||
                                             p.type.id == typeid (ixx) ||
                                             p.type.id == typeid (txx)))
            continue;

          if (p.type.id == typeid (cxx))
          {
            // We need to make sure they are the same which we can only
            // do by comparing the targets to which they resolve.
            //
            target* t (p.target != nullptr ? p.target : &search (p));
            target* ct (cp.target != nullptr ? cp.target : &search (cp));

            if (t == ct)
            {
              add = false;
              continue; // Check the rest of the prerequisites.
            }
          }

          cerr << "error: synthesized target for prerequisite " << cp
               << " would be incompatible with existing target " << ot
               << endl;

          if (p.type.id == typeid (cxx))
            cerr << "info: existing prerequsite " << p << " does not "
                 << "match " << cp << endl;
          else
            cerr << "info: unknown existing prerequsite " << p << endl;

          cerr << "info: specify corresponding obj{} target explicitly"
               << endl;

          throw error ();
        }

        if (add)
          ot.prerequisites.push_back (cp);

        // Change the exe{} target's prerequsite from cxx{} to obj{}.
        //
        pr = op;
      }

      return &update;
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

      for (const prerequisite& p: t.prerequisites)
      {
        const target& pt (*p.target);

        // Assume all our prerequisites are mtime-based (checked in
        // match()).
        //
        const auto& mtp (dynamic_cast<const mtime_target&> (pt));
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

      // Translate paths to relative (to working directory) ones. This
      // results in easier to read diagnostics.
      //
      path re (translate (e.path ()));
      vector<path> ro;

      vector<const char*> args {"g++-4.9", "-std=c++14", "-g", "-o"};

      args.push_back (re.string ().c_str ());

      for (const prerequisite& p: t.prerequisites)
      {
        const obj& o (dynamic_cast<const obj&> (*p.target));
        ro.push_back (translate (o.path ()));
        args.push_back (ro.back ().string ().c_str ());
      }

      args.push_back (nullptr);

      if (verb >= 1)
        print_process (args);
      else
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

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child ())
          exit (1);

        return target_state::failed;
      }
    }
  }
}
