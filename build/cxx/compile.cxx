// file      : build/cxx/compile.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/compile>

#include <map>
#include <string>
#include <cstddef>  // size_t
#include <cstdlib>  // exit()
#include <utility>  // move()

#include <butl/process>
#include <butl/utility>  // reverse_iterate
#include <butl/fdstream>
#include <butl/path-map>

#include <build/types>
#include <build/scope>
#include <build/variable>
#include <build/algorithm>
#include <build/diagnostics>
#include <build/context>

#include <build/bin/target>
#include <build/cxx/target>

#include <build/cxx/utility>
#include <build/cxx/link>

using namespace std;
using namespace butl;

namespace build
{
  namespace cxx
  {
    using namespace bin;

    match_result compile::
    match (action a, target& t, const string&) const
    {
      tracer trace ("cxx::compile::match");

      // @@ TODO:
      //
      // - check prerequisites: single source file
      // - check prerequisites: the rest are headers (other ignorable?)
      // - if path already assigned, verify extension?
      //

      // See if we have a C++ source file. Iterate in reverse so that
      // a source file specified for an obj*{} member overrides the one
      // specified for the group. Also "see through" groups.
      //
      for (prerequisite_member p: reverse_group_prerequisite_members (a, t))
      {
        if (p.is_a<cxx> ())
          return p;
      }

      level3 ([&]{trace << "no c++ source file for target " << t;});
      return nullptr;
    }

    static void
    inject_prerequisites (action, target&, cxx&, scope&);

    recipe compile::
    apply (action a, target& xt, const match_result& mr) const
    {
      path_target& t (static_cast<path_target&> (xt));

      // Derive file name from target name.
      //
      if (t.path ().empty ())
        t.derive_path ("o", nullptr, (t.is_a<objso> () ? "-so" : nullptr));

      // Inject dependency on the output directory.
      //
      inject_parent_fsdir (a, t);

      // Search and match all the existing prerequisites. The injection
      // code (below) takes care of the ones it is adding.
      //
      // When cleaning, ignore prerequisites that are not in the same
      // or a subdirectory of our strong amalgamation.
      //
      const dir_path* amlg (
        a.operation () != clean_id
        ? nullptr
        : &t.strong_scope ().path ());

      link::search_paths_cache lib_paths; // Extract lazily.

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // A dependency on a library is there so that we can get its
        // cxx.export.poptions. In particular, making sure it is
        // executed before us will only restrict parallelism. But we
        // do need to match it in order to get its prerequisite_targets
        // populated; see append_lib_options() above.
        //
        if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libso> ())
        {
          if (a.operation () == update_id)
          {
            // Handle imported libraries.
            //
            if (p.proj () != nullptr)
            {
              // We know that for such libraries we don't need to do
              // match() in order to get options (if any, they would
              // be set by search_library()).
              //
              if (link::search_library (lib_paths, p.prerequisite) != nullptr)
                continue;
            }

            target& pt (p.search ());

            // @@ The fact that we match but never execute messes up
            //    the dependents count. This is a workaround, not a
            //    solution.
            //
            build::match (a, pt);
          }

          continue;
        }

        target& pt (p.search ());

        if (a.operation () == clean_id && !pt.dir.sub (*amlg))
          continue;

        build::match (a, pt);
        t.prerequisite_targets.push_back (&pt);
      }

      // Inject additional prerequisites. We only do it for update
      // since chances are we will have to update some of our
      // prerequisites in the process (auto-generated source code).
      //
      if (a.operation () == update_id)
      {
        // The cached prerequisite target should be the same as what
        // is in t.prerequisite_targets since we used standard
        // search() and match() above.
        //
        // @@ Ugly.
        //
        cxx& st (
          dynamic_cast<cxx&> (
            mr.target != nullptr ? *mr.target : *mr.prerequisite->target));
        inject_prerequisites (a, t, st, mr.prerequisite->scope);
      }

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id: return &perform_clean;
      default: return default_recipe; // Forward to prerequisites.
      }
    }

    // The strings used as the map key should be from the extension_pool.
    // This way we can just compare pointers.
    //
    using ext_map = map<const string*, const target_type*>;

    static ext_map
    build_ext_map (scope& r)
    {
      ext_map m;

      if (auto val = r["h.ext"])
        m[&extension_pool.find (val.as<const string&> ())] = &h::static_type;

      if (auto val = r["c.ext"])
        m[&extension_pool.find (val.as<const string&> ())] = &c::static_type;

      if (auto val = r["hxx.ext"])
        m[&extension_pool.find (val.as<const string&> ())] = &hxx::static_type;

      if (auto val = r["ixx.ext"])
        m[&extension_pool.find (val.as<const string&> ())] = &ixx::static_type;

      if (auto val = r["txx.ext"])
        m[&extension_pool.find (val.as<const string&> ())] = &txx::static_type;

      if (auto val = r["cxx.ext"])
        m[&extension_pool.find (val.as<const string&> ())] = &cxx::static_type;

      return m;
    }

    // Mapping of include prefixes (e.g., foo in <foo/bar>) for auto-
    // generated headers to directories where they will be generated.
    //
    // We are using a prefix map of directories (dir_path_map) instead
    // of just a map in order also cover sub-paths (e.g., <foo/more/bar>
    // if we continue with the example). Specifically, we need to make
    // sure we don't treat foobar as a sub-directory of foo.
    //
    // @@ The keys should be canonicalized.
    //
    using prefix_map = dir_path_map<dir_path>;

    static void
    append_prefixes (prefix_map& m, target& t, const char* var)
    {
      tracer trace ("cxx::append_prefixes");

      const dir_path& out_base (t.dir);
      const dir_path& out_root (t.root_scope ().path ());

      if (auto val = t[var])
      {
        const list_value& l (val.template as<const list_value&> ());

        // Assume the names have already been vetted by append_options().
        //
        for (auto i (l.begin ()), e (l.end ()); i != e; ++i)
        {
          // -I can either be in the -Ifoo or -I foo form.
          //
          dir_path d;
          if (i->value == "-I")
          {
            if (++i == e)
              break; // Let the compiler complain.

            d = i->simple () ? dir_path (i->value) : i->dir;
          }
          else if (i->value.compare (0, 2, "-I") == 0)
            d = dir_path (i->value, 2, string::npos);
          else
            continue;

          level5 ([&]{trace << "-I '" << d << "'";});

          // If we are relative or not inside our project root, then
          // ignore.
          //
          if (d.relative () || !d.sub (out_root))
            continue;

          // If the target directory is a sub-directory of the include
          // directory, then the prefix is the difference between the
          // two. Otherwise, leave it empty.
          //
          // The idea here is to make this "canonical" setup work auto-
          // magically:
          //
          // 1. We include all files with a prefix, e.g., <foo/bar>.
          // 2. The library target is in the foo/ sub-directory, e.g.,
          //    /tmp/foo/.
          // 3. The poptions variable contains -I/tmp.
          //
          dir_path p (out_base.sub (d) ? out_base.leaf (d) : dir_path ());

          auto j (m.find (p));

          if (j != m.end ())
          {
            if (j->second != d)
              fail << "duplicate generated dependency prefix '" << p << "'" <<
                info << "old mapping to " << j->second <<
                info << "new mapping to " << d;
          }
          else
          {
            level5 ([&]{trace << "'" << p << "' = '" << d << "'";});
            m.emplace (move (p), move (d));
          }
        }
      }
    }

    // Append library prefixes based on the cxx.export.poptions variables
    // recursively, prerequisite libraries first.
    //
    static void
    append_lib_prefixes (prefix_map& m, target& l)
    {
      for (target* t: l.prerequisite_targets)
      {
        if (t == nullptr)
          continue;

        if (t->is_a<lib> () || t->is_a<liba> () || t->is_a<libso> ())
          append_lib_prefixes (m, *t);
      }

      append_prefixes (m, l, "cxx.export.poptions");
    }

    static prefix_map
    build_prefix_map (target& t)
    {
      prefix_map m;

      // First process the include directories from prerequisite
      // libraries. Note that here we don't need to see group
      // members (see apply()).
      //
      for (prerequisite& p: group_prerequisites (t))
      {
        target& pt (*p.target); // Already searched and matched.

        if (pt.is_a<lib> () || pt.is_a<liba> () || pt.is_a<libso> ())
          append_lib_prefixes (m, pt);
      }

      // Then process our own.
      //
      append_prefixes (m, t, "cxx.poptions");

      return m;
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

    static void
    inject_prerequisites (action a, target& t, cxx& s, scope& ds)
    {
      tracer trace ("cxx::compile::inject_prerequisites");

      scope& rs (t.root_scope ());
      const string& cxx (rs["config.cxx"].as<const string&> ());

      cstrings args {cxx.c_str ()};

      // Add cxx.export.poptions from prerequisite libraries. Note
      // that here we don't need to see group members (see apply()).
      //
      for (prerequisite& p: group_prerequisites (t))
      {
        target& pt (*p.target); // Already searched and matched.

        if (pt.is_a<lib> () || pt.is_a<liba> () || pt.is_a<libso> ())
          append_lib_options (args, pt, "cxx.export.poptions");
      }

      append_options (args, t, "cxx.poptions");

      // @@ Some C++ options (e.g., -std, -m) affect the preprocessor.
      // Or maybe they are not C++ options? Common options?
      //
      append_options (args, t, "cxx.coptions");

      string std; // Storage.
      append_std (args, t, std);

      if (t.is_a<objso> ())
        args.push_back ("-fPIC");

      args.push_back ("-M");  // Note: -MM -MG skips missing <>-included.
      args.push_back ("-MG"); // Treat missing headers as generated.
      args.push_back ("-MQ"); // Quoted target name.
      args.push_back ("*");   // Old versions can't handle empty target name.

      // We are using absolute source file path in order to get absolute
      // paths in the result. Any relative paths in the result are non-
      // existent, potentially auto-generated headers.
      //
      // @@ We will also have to use absolute -I paths to guarantee
      // that. Or just detect relative paths and error out?
      //
      args.push_back (s.path ().string ().c_str ());
      args.push_back (nullptr);

      level5 ([&]{trace << "target: " << t;});

      // Build the prefix map lazily only if we have non-existent files.
      // Also reuse it over restarts since it doesn't change.
      //
      prefix_map pm;

      // If any prerequisites that we have extracted changed, then we
      // have to redo the whole thing. The reason for this is auto-
      // generated headers: the updated header may now include a yet-
      // non-existent header. Unless we discover this and generate it
      // (which, BTW, will trigger another restart since that header,
      // in turn, can also include auto-generated headers), we will
      // end up with an error during compilation proper.
      //
      // One complication with this restart logic is that we will see
      // a "prefix" of prerequisites that we have already processed
      // (i.e., they are already in our prerequisite_targets list) and
      // we don't want to keep redoing this over and over again. One
      // thing to note, however, is that the prefix that we have seen
      // on the previous run must appear exactly the same in the
      // subsequent run. The reason for this is that none of the files
      // that it can possibly be based on have changed and thus it
      // should be exactly the same. To put it another way, the
      // presence or absence of a file in the dependency output can
      // only depend on the previous files (assuming the compiler
      // outputs them as it encounters them and it is hard to think
      // of a reason why would someone do otherwise). And we have
      // already made sure that all those files are up to date. And
      // here is the way we are going to exploit this: we are going
      // to keep track of how many prerequisites we have processed so
      // far and on restart skip right to the next one.
      //
      // Also, before we do all that, make sure the source file itself
      // if up to date.
      //
      execute_direct (a, s);

      size_t skip_count (0);
      for (bool restart (true); restart; )
      {
        restart = false;

        if (verb >= 2)
          print_process (args);

        try
        {
          process pr (args.data (), 0, -1); // Open pipe to stdout.
          ifdstream is (pr.in_ofd);

          size_t skip (skip_count);
          for (bool first (true), second (true); !(restart || is.eof ()); )
          {
            string l;
            getline (is, l);

            if (is.fail () && !is.eof ())
              fail << "error reading C++ compiler -M output";

            size_t pos (0);

            if (first)
            {
              // Empty output should mean the wait() call below will return
              // false.
              //
              if (l.empty ())
                break;

              assert (l[0] == '*' && l[1] == ':' && l[2] == ' ');

              first = false;

              // While normally we would have the source file on the
              // first line, if too long, it will be moved to the next
              // line and all we will have on this line is "*: \".
              //
              if (l.size () == 4 && l[3] == '\\')
                continue;
              else
                pos = 3; // Skip "*: ".

              // Fall through to the 'second' block.
            }

            if (second)
            {
              second = false;
              next (l, pos); // Skip the source file.
            }

            // If things go wrong (and they often do in this area), give
            // the user a bit extra context.
            //
            auto g (
              make_exception_guard (
                [](target& s)
                {
                  info << "while extracting dependencies from " << s;
                },
                s));

            while (pos != l.size ())
            {
              string fs (next (l, pos));

              // Skip until where we left off.
              //
              if (skip != 0)
              {
                skip--;
                continue;
              }

              path f (move (fs));
              f.normalize ();

              if (!f.absolute ())
              {
                // This is probably as often an error as an auto-generated
                // file, so trace at level 3.
                //
                level3 ([&]{trace << "non-existent header '" << f << "'";});

                // If we already did it and build_prefix_map() returned empty,
                // then we would have failed below.
                //
                if (pm.empty ())
                  pm = build_prefix_map (t);

                // First try the whole file. Then just the directory.
                //
                // @@ Has to be a separate map since the prefix can be
                //    the same as the file name.
                //
                // auto i (pm.find (f));

                // Find the most qualified prefix of which we are a
                // sub-path.
                //
                auto i (pm.end ());

                if (!pm.empty ())
                {
                  const dir_path& d (f.directory ());
                  i = pm.upper_bound (d);
                  --i; // Greatest less than.

                  if (!d.sub (i->first)) // We might still not be a sub.
                    i = pm.end ();
                }

                if (i == pm.end ())
                  fail << "unable to map presumably auto-generated header '"
                       << f << "' to a project";

                f = i->second / f;
              }

              level5 ([&]{trace << "injecting " << f;});

              // Split the name into its directory part, the name part, and
              // extension. Here we can assume the name part is a valid
              // filesystem name.
              //
              // Note that if the file has no extension, we record an empty
              // extension rather than NULL (which would signify that the
              // default extension should be added).
              //
              dir_path d (f.directory ());
              string n (f.leaf ().base ().string ());
              const char* es (f.extension ());
              const string* e (&extension_pool.find (es != nullptr ? es : ""));

              // Determine the target type.
              //
              const target_type* tt (nullptr);

              // See if this directory is part of any project out_root
              // hierarchy. Note that this will miss all the headers
              // that come from src_root (so they will be treated as
              // generic C headers below). Generally, we don't have
              // the ability to determine that some file belongs to
              // src_root of some project. But that's not a problem
              // for our purposes: it is only important for us to
              // accurately determine target types for headers that
              // could be auto-generated.
              //
              if (scope* r = scopes.find (d).root_scope ())
              {
                // Get cached (or build) a map of the extensions for the
                // C/C++ files this project is using.
                //
                const ext_map& m (build_ext_map (*r));

                auto i (m.find (e));
                if (i != m.end ())
                  tt = i->second;
              }

              // If it is outside any project, or the project doesn't have
              // such an extension, assume it is a plain old C header.
              //
              if (tt == nullptr)
                tt = &h::static_type;

              // Find or insert target.
              //
              path_target& pt (
                static_cast<path_target&> (search (*tt, d, n, e, &ds)));

              // Assign path.
              //
              if (pt.path ().empty ())
                pt.path (move (f));

              // Match to a rule.
              //
              build::match (a, pt);

              // Update it.
              //
              // There would normally be a lot of headers for every source
              // file (think all the system headers) and this can get
              // expensive. At the same time, most of these headers are
              // existing files that we will never be updating (again,
              // system headers, for example) and the rule that will match
              // them is fallback file_rule. So we are going to do a little
              // fast-path optimization by detecting this common case.
              //
              if (!file_rule::uptodate (a, pt))
              {
                // We only want to restart if our call to execute() actually
                // caused an update. In particular, the target could already
                // have been in target_state::changed because of a dependency
                // extraction run for some other source file.
                //
                target_state os (pt.state ());
                target_state ns (execute_direct (a, pt));

                if (ns != os && ns != target_state::unchanged)
                {
                  level5 ([&]{trace << "updated " << pt << ", restarting";});
                  restart = true;
                }
              }

              // Add to our prerequisite target list.
              //
              t.prerequisite_targets.push_back (&pt);
              skip_count++;
            }
          }

          // We may not have read all the output (e.g., due to a restart),
          // so close the file descriptor before waiting to avoid blocking
          // the other end.
          //
          is.close ();

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
    }

    target_state compile::
    perform_update (action a, target& xt)
    {
      path_target& t (static_cast<path_target&> (xt));
      cxx* s (execute_prerequisites<cxx> (a, t, t.mtime ()));

      if (s == nullptr)
        return target_state::unchanged;

      // Translate paths to relative (to working directory) ones. This
      // results in easier to read diagnostics.
      //
      path relo (relative (t.path ()));
      path rels (relative (s->path ()));

      scope& rs (t.root_scope ());
      const string& cxx (rs["config.cxx"].as<const string&> ());

      cstrings args {cxx.c_str ()};

      // Add cxx.export.poptions from prerequisite libraries. Note that
      // here we don't need to see group members (see apply()).
      //
      for (prerequisite& p: group_prerequisites (t))
      {
        target& pt (*p.target); // Already searched and matched.

        if (pt.is_a<lib> () || pt.is_a<liba> () || pt.is_a<libso> ())
          append_lib_options (args, pt, "cxx.export.poptions");
      }

      append_options (args, t, "cxx.poptions");
      append_options (args, t, "cxx.coptions");

      string std; // Storage.
      append_std (args, t, std);

      if (t.is_a<objso> ())
        args.push_back ("-fPIC");

      args.push_back ("-o");
      args.push_back (relo.string ().c_str ());

      args.push_back ("-c");
      args.push_back (rels.string ().c_str ());

      args.push_back (nullptr);

      if (verb)
        print_process (args);
      else
        text << "c++ " << *s;

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
        t.mtime (system_clock::now ());
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
