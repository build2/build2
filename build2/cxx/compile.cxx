// file      : build2/cxx/compile.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/compile>

#include <map>
#include <cstdlib>  // exit()

#include <butl/process>
#include <butl/fdstream>
#include <butl/path-map>

#include <build2/scope>
#include <build2/variable>
#include <build2/algorithm>
#include <build2/diagnostics>
#include <build2/context>

#include <build2/bin/target>
#include <build2/cxx/target>

#include <build2/cxx/utility>
#include <build2/cxx/link>

using namespace std;
using namespace butl;

namespace build2
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

      level4 ([&]{trace << "no c++ source file for target " << t;});
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
      // or a subdirectory of ours.
      //
      link::search_paths_cache lib_paths; // Extract lazily.

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // A dependency on a library is there so that we can get its
        // cxx.export.poptions. In particular, making sure it is
        // executed before us will only restrict parallelism. But we
        // do need to pre-match it in order to get its
        // prerequisite_targets populated. This is the "library
        // meta-information protocol". See also append_lib_options()
        // above.
        //
        if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libso> ())
        {
          if (a.operation () == update_id)
          {
            // Handle imported libraries. We know that for such libraries
            // we don't need to do match() in order to get options (if
            // any, they would be set by search_library()).
            //
            if (p.proj () == nullptr ||
                link::search_library (lib_paths, p.prerequisite) == nullptr)
            {
              match_only (a, p.search ());
            }
          }

          continue;
        }

        target& pt (p.search ());

        if (a.operation () == clean_id && !pt.dir.sub (t.dir))
          continue;

        build2::match (a, pt);
        t.prerequisite_targets.push_back (&pt);
      }

      // Inject additional prerequisites. We only do it when
      // performing update since chances are we will have to
      // update some of our prerequisites in the process (auto-
      // generated source code).
      //
      if (a == perform_update_id)
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
      default: return noop_recipe; // Configure update.
      }
    }

    // Reverse-lookup target type from extension.
    //
    static const target_type*
    map_extension (scope& s, const string& n, const string& e)
    {
      // We will just have to try all of the possible ones, in the
      // "most likely to match" order.
      //
      const variable& var (var_pool.find ("extension"));

      auto test = [&s, &n, &e, &var] (const target_type& tt)
        -> const target_type*
      {
        if (auto l = s.lookup (tt, n, var))
          if (as<string> (*l) == e)
            return &tt;

        return nullptr;
      };

      if (auto r = test (hxx::static_type)) return r;
      if (auto r = test (h::static_type))   return r;
      if (auto r = test (ixx::static_type)) return r;
      if (auto r = test (txx::static_type)) return r;
      if (auto r = test (cxx::static_type)) return r;
      if (auto r = test (c::static_type))   return r;

      return nullptr;
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

      // If this target does not belong to any project (e.g, an
      // "imported as installed" library), then it can't possibly
      // generate any headers for us.
      //
      scope* rs (t.base_scope ().root_scope ());
      if (rs == nullptr)
        return;

      const dir_path& out_base (t.dir);
      const dir_path& out_root (rs->out_path ());

      if (auto l = t[var])
      {
        const auto& v (as<strings> (*l));

        for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
        {
          // -I can either be in the "-Ifoo" or "-I foo" form.
          //
          dir_path d;
          if (*i == "-I")
          {
            if (++i == e)
              break; // Let the compiler complain.

            d = dir_path (*i);
          }
          else if (i->compare (0, 2, "-I") == 0)
            d = dir_path (*i, 2, string::npos);
          else
            continue;

          level6 ([&]{trace << "-I '" << d << "'";});

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
            {
              // We used to reject duplicates but it seems this can
              // be reasonably expected to work according to the order
              // of the -I options.
              //
              if (verb >= 4)
                trace << "overriding dependency prefix '" << p << "'\n"
                      << "  old mapping to " << j->second << "\n"
                      << "  new mapping to " << d;

              j->second = d;
            }
          }
          else
          {
            level6 ([&]{trace << "'" << p << "' = '" << d << "'";});
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
      const string& cxx (as<string> (*rs["config.cxx"]));
      const string& sys (as<string> (*rs["cxx.host.system"]));

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
      {
        if (sys != "darwin") // fPIC by default.
          args.push_back ("-fPIC");
      }

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

      level6 ([&]{trace << "target: " << t;});

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

        if (verb >= 3)
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
                [&s]()
                {
                  info << "while extracting dependencies from " << s;
                }));

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

              if (!f.absolute ())
              {
                f.normalize ();

                // This is probably as often an error as an auto-generated
                // file, so trace at level 4.
                //
                level4 ([&]{trace << "non-existent header '" << f << "'";});

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

                  // Get the greatest less than, if any. We might
                  // still not be a sub. Note also that we still
                  // have to check the last element is upper_bound()
                  // returned end().
                  //
                  if (i == pm.begin () || !d.sub ((--i)->first))
                    i = pm.end ();
                }

                if (i == pm.end ())
                  fail << "unable to map presumably auto-generated header '"
                       << f << "' to a project";

                f = i->second / f;
              }
              else
              {
                // We used to just normalize the path but that could result in
                // an invalid path (e.g., on CentOS 7 with Clang 3.4) because
                // of the symlinks. So now we realize (i.e., realpath(3)) it
                // instead.
                //
                f.realize ();
              }

              level6 ([&]{trace << "injecting " << f;});

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
              scope& b (scopes.find (d));
              if (b.root_scope () != nullptr)
                tt = map_extension (b, n, *e);

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
              build2::match (a, pt);

              // Update it.
              //
              // There would normally be a lot of headers for every source
              // file (think all the system headers) and this can get
              // expensive. At the same time, most of these headers are
              // existing files that we will never be updated (again,
              // system headers, for example) and the rule that will match
              // them is fallback file_rule. That rule has an optimization
              // in that it returns noop_recipe (which causes the target
              // state to be automatically set to unchanged) if the file
              // is known to be up to date.
              //
              if (pt.state () != target_state::unchanged)
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
                  level6 ([&]{trace << "updated " << pt << ", restarting";});
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
          {
            // In case of a restarts, we closed our end of the pipe early
            // which might have caused the other end to fail. So far we
            // experienced this on Fedora 23 with GCC 5.3.1 and there were
            // no diagnostics issued, just the non-zero exit status. If we
            // do get diagnostics, then we will have to read and discard the
            // output until eof.
            //
            if (!restart)
              throw failed ();
          }
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
      const string& cxx (as<string> (*rs["config.cxx"]));
      const string& sys (as<string> (*rs["cxx.host.system"]));

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
      {
        if (sys != "darwin") // fPIC by default.
          args.push_back ("-fPIC");
      }

      args.push_back ("-o");
      args.push_back (relo.string ().c_str ());

      args.push_back ("-c");
      args.push_back (rels.string ().c_str ());

      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);
      else if (verb)
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

    compile compile::instance;
  }
}
