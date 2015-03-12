// file      : build/cxx/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/rule>

#include <string>
#include <vector>
#include <cstddef>  // size_t
#include <cstdlib>  // exit
#include <utility>  // move()

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
    match (action a, target& t, const string&) const
    {
      tracer trace ("cxx::compile::match");

      // @@ TODO:
      //
      // - check prerequisites: single source file
      // - check prerequisites: the rest are headers (other ignorable?)
      // - if path already assigned, verify extension?
      //
      // @@ Q:
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

      level3 ([&]{trace << "no c++ source file for target " << t;});
      return nullptr;
    }

    recipe compile::
    apply (action a, target& t, void* v) const
    {
      // Derive object file name from target name.
      //
      obj& o (dynamic_cast<obj&> (t));

      if (o.path ().empty ())
        o.path (o.dir / path (o.name + ".o"));

      // Search and match all the existing prerequisites. The injection
      // code (below) takes care of the ones it is adding.
      //
      // When cleaning, ignore prerequisites that are not in the same
      // or a subdirectory of ours.
      //
      switch (a.operation ())
      {
      case update_id: search_and_match (a, t); break;
      case clean_id:  search_and_match (a, t, t.dir); break;
      default:        assert (false);
      }

      // Inject additional prerequisites. For now we only do it for
      // update.
      //
      if (a.operation () == update_id)
      {
        auto& sp (*static_cast<prerequisite*> (v));
        auto& st (dynamic_cast<cxx&> (*sp.target));

        if (st.mtime () != timestamp_nonexistent)
          inject_prerequisites (a, o, st, sp.scope);
      }

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id: return &perform_clean_file;
      default: return noop_recipe;
      }
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
    inject_prerequisites (action a, obj& o, const cxx& s, scope& ds) const
    {
      tracer trace ("cxx::compile::inject_prerequisites");

      // We are using absolute source file path in order to get
      // absolute paths in the result. @@ We will also have to
      // use absolute -I paths to guarantee that.
      //
      const char* args[] = {
        "g++-4.9",
        "-std=c++14",
        "-I", ds["src_root"].as<const path&> ().string ().c_str (),
        "-MM",       //@@ -M
        "-MG",      // Treat missing headers as generated.
        "-MQ", "*", // Quoted target (older version can't handle empty name).
        s.path ().string ().c_str (),
        nullptr};

      if (verb >= 2)
        print_process (args);

      level5 ([&]{trace << "target: " << o;});

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
            fail << "io error while parsing g++ -M output";

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
            path f (next (l, pos));
            f.normalize ();

            assert (f.absolute ()); // Logic below depends on this.

            level5 ([&]{trace << "prerequisite path: " << f.string ();});

            // Split the name into its directory part, the name part, and
            // extension. Here we can assume the name part is a valid
            // filesystem name.
            //
            // Note that if the file has no extension, we record an empty
            // extension rather than NULL (which would signify that the
            // extension needs to be added).
            //
            path d (f.directory ());
            string n (f.leaf ().base ().string ());
            const char* es (f.extension ());
            const string* e (&extension_pool.find (es != nullptr ? es : ""));

            // Find or insert prerequisite.
            //
            // If there is no extension (e.g., standard C++ headers),
            // then assume it is a header. Otherwise, let the standard
            // mechanism derive the type from the extension. @@ TODO.
            //
            prerequisite& p (
              ds.prerequisites.insert (
                hxx::static_type, move (d), move (n), e, ds, trace).first);

            o.prerequisites.push_back (p);

            // Resolve to target.
            //
            path_target& t (dynamic_cast<path_target&> (search (p)));

            // Assign path.
            //
            if (t.path ().empty ())
              t.path (move (f));

            // Match to a rule.
            //
            build::match (a, t);
          }
        }

        // We assume the child process issued some diagnostics.
        //
        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    target_state compile::
    perform_update (action a, target& t)
    {
      obj& o (dynamic_cast<obj&> (t));
      cxx* s (execute_prerequisites<cxx> (a, o, o.mtime ()));

      if (s == nullptr)
        return target_state::unchanged;

      // Translate paths to relative (to working directory) ones. This
      // results in easier to read diagnostics.
      //
      path ro (relative_work (o.path ()));
      path rs (relative_work (s->path ()));

      const char* args[] = {
        "g++-4.9",
        "-std=c++14",
        "-g",
        "-I", o.prerequisites[0].get ().scope["src_root"].as<const path&> ().string ().c_str (),
        "-c",
        "-o", ro.string ().c_str (),
        rs.string ().c_str (),
        nullptr};

      if (verb >= 1)
        print_process (args);
      else
        text << "c++ " << *s;

      try
      {
        process pr (args);

        if (!pr.wait ())
          throw failed ();

        // Should we go to the filesystem and get the new mtime? We
        // know the file has been modified, so instead just use the
        // current clock time. It has the advantage of having the
        // subseconds precision.
        //
        o.mtime (system_clock::now ());
        return target_state::changed;
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    // link
    //
    void* link::
    match (action a, target& t, const string& hint) const
    {
      tracer trace ("cxx::link::match");

      // @@ TODO:
      //
      // - check prerequisites: object files, libraries
      // - if path already assigned, verify extension?
      //
      // @@ Q:
      //
      // - if there is no .o, are we going to check if the one derived
      //   from target exist or can be built? A: No.
      //   What if there is a library. Probably ok if .a, not if .so.
      //   (i.e., a utility library).
      //

      // Scan prerequisites and see if we can work with what we've got.
      //
      bool seen_cxx (false), seen_c (false), seen_obj (false);

      for (prerequisite& p: t.prerequisites)
      {
        if (p.type.id == typeid (cxx)) // @@ Should use is_a (add to p.type).
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
          level3 ([&]{trace << "unexpected prerequisite type " << p.type;});
          return nullptr;
        }
      }

      // We will only chain a C source if there is also a C++ source or we
      // were explicitly told to.
      //
      if (seen_c && !seen_cxx && hint < "cxx")
      {
        level3 ([&]{trace << "c prerequisite(s) without c++ or hint";});
        return nullptr;
      }

      return seen_cxx || seen_c || seen_obj ? &t : nullptr;
    }

    recipe link::
    apply (action a, target& t, void*) const
    {
      tracer trace ("cxx::link::apply");

      // Derive executable file name from target name.
      //
      exe& e (dynamic_cast<exe&> (t));

      if (e.path ().empty ())
        e.path (e.dir / path (e.name));

      // We may need the project roots for rule chaining (see below).
      // We will resolve them lazily only if needed.
      //
      const path* out_root (nullptr);
      const path* src_root (nullptr);

      // Process prerequisites: do rule chaining for C and C++ source
      // files as well as search and match.
      //
      for (auto& pr: t.prerequisites)
      {
        prerequisite& p (pr);

        if (p.type.id != typeid (c) && p.type.id != typeid (cxx))
        {
          // The same logic as in search_and_match().
          //
          target& pt (search (p));

          if (a.operation () == clean_id && !pt.dir.sub (e.dir))
            p.target = nullptr; // Ignore.
          else
            build::match (a, pt);

          continue;
        }

        if (out_root == nullptr)
        {
          // Which scope shall we use to resolve the roots? Unlikely,
          // but possible, the prerequisite is from a different project
          // altogether. So we are going to use the target's project.
          //
          scope& s (scopes.find (e.dir));
          out_root = &s["out_root"].as<const path&> ();
          src_root = &s["src_root"].as<const path&> ();
        }

        prerequisite& cp (p);

        // Come up with the obj{} prerequisite. The c(xx){} prerequisite
        // directory can be relative (to the scope) or absolute. If it is
        // relative, then use it as is. If it is absolute, then translate
        // it to the corresponding directory under out_root. While the
        // c(xx){} directory is most likely under src_root, it is also
        // possible it is under out_root (e.g., generated source).
        //
        path d;
        if (cp.dir.relative () || cp.dir.sub (*out_root))
          d = cp.dir;
        else
        {
          if (!cp.dir.sub (*src_root))
            fail << "out of project prerequisite " << cp <<
              info << "specify corresponding obj{} target explicitly";

          d = *out_root / cp.dir.leaf (*src_root);
        }

        prerequisite& op (
          cp.scope.prerequisites.insert (
            obj::static_type,
            move (d),
            cp.name,
            nullptr,
            cp.scope,
            trace).first);

        // Resolve this prerequisite to target.
        //
        target& ot (search (op));

        // If we are cleaning, check that this target is in the same or
        // a subdirectory of ours.
        //
        // If it is not, then we are effectively leaving the prerequisites
        // half-rewritten (we only rewrite those that we should clean).
        // What will happen if, say, after clean we have update? Well,
        // update will come and finish the rewrite process (it will even
        // reuse op that we have created but then ignored). So all is good.
        //
        if (a.operation () == clean_id && !ot.dir.sub (e.dir))
        {
          // If we shouldn't clean obj{}, then it is fair to assume
          // we shouldn't clean cxx{} either (generated source will
          // be in the same directory as obj{} and if not, well, go
          // and find yourself another build system).
          //
          p.target = nullptr; // Skip.
          continue;
        }

        // If this target already exists, then it needs to be "compatible"
        // with what we are doing here.
        //
        // This gets a bit tricky. We need to make sure the source files
        // are the same which we can only do by comparing the targets to
        // which they resolve. But we cannot search the ot's prerequisites
        // -- only the rule that matches can. Note, however, that if all
        // this works out, then our next step is to search and match the
        // re-written prerequisite (which points to ot). If things don't
        // work out, then we fail, in which case searching and matching
        // speculatively doesn't really hurt.
        //
        prerequisite* cp1 (nullptr);
        for (prerequisite& p: ot.prerequisites)
        {
          // Ignore some known target types (headers).
          //
          if (p.type.id == typeid (h) ||
              (cp.type.id == typeid (cxx) && (p.type.id == typeid (hxx) ||
                                              p.type.id == typeid (ixx) ||
                                              p.type.id == typeid (txx))))
            continue;

          if (p.type.id == typeid (cxx))
          {
            cp1 = &p; // Check the rest of the prerequisites.
            continue;
          }

          fail << "synthesized target for prerequisite " << cp
               << " would be incompatible with existing target " << ot <<
            info << "unknown existing prerequsite type " << p <<
            info << "specify corresponding obj{} target explicitly";
        }

        if (cp1 != nullptr)
        {
          build::match (a, ot); // Now cp1 should be resolved.
          search (cp);          // Our own prerequisite, so this is ok.

          if (cp.target != cp1->target)
            fail << "synthesized target for prerequisite " << cp
                 << " would be incompatible with existing target " << ot <<
              info << "existing prerequsite " << *cp1 << " does not "
                 << "match " << cp <<
              info << "specify corresponding obj{} target explicitly";
        }
        else
        {
          ot.prerequisites.push_back (cp);
          build::match (a, ot);
        }

        // Change the exe{} target's prerequsite from cxx{} to obj{}.
        //
        pr = op;
      }

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id: return &perform_clean_file;
      default: return noop_recipe;
      }
    }

    target_state link::
    perform_update (action a, target& t)
    {
      // @@ Q:
      //
      // - what are we doing with libraries?
      //

      exe& e (dynamic_cast<exe&> (t));

      if (!execute_prerequisites (a, e, e.mtime ()))
        return target_state::unchanged;

      // Translate paths to relative (to working directory) ones. This
      // results in easier to read diagnostics.
      //
      path re (relative_work (e.path ()));
      vector<path> ro;

      vector<const char*> args {"g++-4.9", "-std=c++14", "-g", "-o"};

      args.push_back (re.string ().c_str ());

      for (const prerequisite& p: t.prerequisites)
      {
        const obj& o (dynamic_cast<const obj&> (*p.target));
        ro.push_back (relative_work (o.path ()));
        args.push_back (ro.back ().string ().c_str ());
      }

      args.push_back (nullptr);

      if (verb >= 1)
        print_process (args);
      else
        text << "ld " << e;

      try
      {
        process pr (args.data ());

        if (!pr.wait ())
          throw failed ();

        // Should we go to the filesystem and get the new mtime? We
        // know the file has been modified, so instead just use the
        // current clock time. It has the advantage of having the
        // subseconds precision.
        //
        e.mtime (system_clock::now ());
        return target_state::changed;
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child ())
          exit (1);

        throw failed ();
      }
    }
  }
}
