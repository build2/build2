// file      : build/cxx/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/rule>

#include <string>
#include <vector>
#include <cstddef>  // size_t
#include <cstdlib>  // exit
#include <utility>  // move()
#include <istream>

#include <ext/stdio_filebuf.h>

#include <build/scope>
#include <build/algorithm>
#include <build/process>
#include <build/timestamp>
#include <build/diagnostics>
#include <build/context>

#include <build/bin/target>

#include <build/cxx/target>

using namespace std;

namespace build
{
  namespace cxx
  {
    using namespace bin;

    // T is either target or scope.
    //
    template <typename T>
    static void
    append_options (vector<const char*>& args, T& s, const char* var)
    {
      if (auto val = s[var])
      {
        for (const name& n: val.template as<const list_value&> ())
        {
          if (!n.type.empty () || !n.dir.empty ())
            fail << "expected option instead of " << n <<
              info << "in variable " << var;

          args.push_back (n.value.c_str ());
        }
      }
    }

    static void
    append_std (vector<const char*>& args, target& t, string& opt)
    {
      if (auto val = t["cxx.std"])
      {
        const string& v (val.as<const string&> ());

        // @@ Need to translate 11 to 0x for older versions.
        //
        opt = "-std=c++" + v;
        args.push_back (opt.c_str ());
      }
    }

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

      // See if we have a C++ source file. Iterate in reverse so that
      // a source file specified for an obj member overrides the one
      // specified for the group.
      //
      for (prerequisite& p: reverse_iterate (group_prerequisites (t)))
      {
        if (p.type.id == typeid (cxx))
          return &p;
      }

      level3 ([&]{trace << "no c++ source file for target " << t;});
      return nullptr;
    }

    recipe compile::
    apply (action a, target& xt, void* v) const
    {
      path_target& t (static_cast<path_target&> (xt));

      // Derive file name from target name.
      //
      if (t.path ().empty ())
      {
        if (t.is_a <obja> ())
          t.path (t.derived_path ("o"));
        else
          t.path (t.derived_path ("o", nullptr, "-so"));
      }

      // Inject dependency on the output directory.
      //
      inject_parent_fsdir (a, t);

      // Search and match all the existing prerequisites. The injection
      // code (below) takes care of the ones it is adding.
      //
      // When cleaning, ignore prerequisites that are not in the same
      // or a subdirectory of ours.
      //
      switch (a.operation ())
      {
      case default_id:
      case update_id: search_and_match (a, t); break;
      case clean_id:  search_and_match (a, t, t.dir); break;
      default:        assert (false);
      }

      // Inject additional prerequisites. For now we only do it for
      // update and default.
      //
      if (a.operation () == update_id || a.operation () == default_id)
      {
        // The cached prerequisite target (sp.target) should be the
        // same as what is in t.prerequisite_targets since we used
        // standard search_and_match() above.
        //
        prerequisite& sp (*static_cast<prerequisite*> (v));
        cxx& st (dynamic_cast<cxx&> (*sp.target));

        if (st.mtime () != timestamp_nonexistent)
          inject_prerequisites (a, t, st, sp.scope);
      }

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id: return &perform_clean;
      default: return default_recipe; // Forward to prerequisites.
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
    inject_prerequisites (action a, target& t, const cxx& s, scope& ds) const
    {
      tracer trace ("cxx::compile::inject_prerequisites");

      scope& rs (*t.root_scope ()); // Shouldn't have matched if nullptr.
      const string& cxx (rs["config.cxx"].as<const string&> ());

      vector<const char*> args {cxx.c_str ()};

      append_options (args, t, "cxx.poptions");

      // @@ Some C++ options (e.g., -std, -m) affect the preprocessor.
      // Or maybe they are not C++ options? Common options?
      //
      append_options (args, t, "cxx.coptions");

      string std; // Storage.
      append_std (args, t, std);

      if (t.is_a<objso> ())
        args.push_back ("-fPIC");

      args.push_back ("-MM"); // @@ Change to -M
      args.push_back ("-MG"); // Treat missing headers as generated.
      args.push_back ("-MQ"); // Quoted target name.
      args.push_back ("*");   // Old versions can't handle empty target name.

      // We are using absolute source file path in order to get
      // absolute paths in the result. Any relative paths in the
      // result are non-existent generated headers.
      //
      // @@ We will also have to use absolute -I paths to guarantee
      // that.
      //
      args.push_back (s.path ().string ().c_str ());

      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);

      level5 ([&]{trace << "target: " << t;});

      try
      {
        process pr (args.data (), false, false, true);

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

            if (!f.absolute ())
            {
              level5 ([&]{trace << "skipping generated/non-existent " << f;});
              continue;
            }

            level5 ([&]{trace << "injecting " << f;});

            // Split the name into its directory part, the name part, and
            // extension. Here we can assume the name part is a valid
            // filesystem name.
            //
            // Note that if the file has no extension, we record an empty
            // extension rather than NULL (which would signify that the
            // default extension needs to be added).
            //
            dir_path d (f.directory ());
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

            // Add to our prerequisites list.
            //
            t.prerequisites.emplace_back (p);

            // Resolve to target.
            //
            path_target& pt (dynamic_cast<path_target&> (search (p)));

            // Assign path.
            //
            if (pt.path ().empty ())
              pt.path (move (f));

            // Match to a rule.
            //
            build::match (a, pt);

            // Add to our resolved target list.
            //
            t.prerequisite_targets.push_back (&pt);
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

      scope& rs (*t.root_scope ()); // Shouldn't have matched if nullptr.
      const string& cxx (rs["config.cxx"].as<const string&> ());

      vector<const char*> args {cxx.c_str ()};

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

      bool so (t.is_a<libso> ());

      // Scan prerequisites and see if we can work with what we've got.
      //
      bool seen_cxx (false), seen_c (false), seen_obj (false),
        seen_lib (false);

      for (prerequisite& p: group_prerequisites (t))
      {
        if (p.type.id == typeid (cxx)) // @@ Should use is_a (add to p.type).
        {
          seen_cxx = seen_cxx || true;
        }
        else if (p.type.id == typeid (c))
        {
          seen_c = seen_c || true;
        }
        else if (p.type.id == typeid (obja))
        {
          if (so)
            fail << "shared library " << t << " prerequisite " << p
                 << " is static object";

          seen_obj = seen_obj || true;
        }
        else if (p.type.id == typeid (objso) || p.type.id == typeid (obj))
        {
          seen_obj = seen_obj || true;
        }
        else if (p.type.id == typeid (liba)  ||
                 p.type.id == typeid (libso) ||
                 p.type.id == typeid (lib))
        {
          seen_lib = seen_lib || true;
        }
        else if (p.type.id != typeid (fsdir))
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

      return seen_cxx || seen_c || seen_obj || seen_lib ? &t : nullptr;
    }

    recipe link::
    apply (action a, target& xt, void*) const
    {
      tracer trace ("cxx::link::apply");

      path_target& t (static_cast<path_target&> (xt));

      type tt (t.is_a<exe> ()
               ? type::exe
               : (t.is_a<liba> () ? type::liba : type::libso));

      bool so (tt == type::libso); // Obj-so.

      // Decide which lib{} member to use for this target.
      //
      bool lso; // Lib-so.
      switch (tt)
      {
      case type::exe:   lso = true; break;
      case type::liba:  lso = false;  break;
      case type::libso: lso = true; break;
      }

      // Derive file name from target name.
      //
      if (t.path ().empty ())
      {
        switch (tt)
        {
        case type::exe:   t.path (t.derived_path (           )); break;
        case type::liba:  t.path (t.derived_path ("a",  "lib")); break;
        case type::libso: t.path (t.derived_path ("so", "lib")); break;
        }
      }

      // Inject dependency on the output directory.
      //
      inject_parent_fsdir (a, t);

      // We may need the project roots for rule chaining (see below).
      // We will resolve them lazily only if needed.
      //
      const dir_path* out_root (nullptr);
      const dir_path* src_root (nullptr);

      // Process prerequisites: do rule chaining for C and C++ source
      // files as well as search and match.
      //
      group_prerequisites gp (t);
      t.prerequisite_targets.reserve (gp.size ());

      for (prerequisite_ref& pr: gp)
      {
        bool group (!pr.belongs (t)); // Target group's prerequisite.

        prerequisite& p (pr);
        target* pt (nullptr);

        if (!p.is_a<c> () && !p.is_a<cxx> ())
        {
          // The same basic logic as in search_and_match().
          //
          pt = &search (p);

          if (a.operation () == clean_id && !pt->dir.sub (t.dir))
            continue; // Skip.

          // If this is the obj{} or lib{} target group, then pick the
          // appropriate member and make sure it is searched and matched.
          //
          if (obj* o = pt->is_a<obj> ())
          {
            pt = so ? static_cast<target*> (o->so) : o->a;

            if (pt == nullptr)
            {
              const target_type& type (
                so ? objso::static_type : obja::static_type);

              pt = &search (
                prerequisite_key {&type, &p.dir, &p.name, &p.ext, &p.scope});
            }
          }
          else if (lib* l = pt->is_a<lib> ())
          {
            // Make sure the library build that we need is available.
            //
            const string& at ((*l)["bin.lib"].as<const string&> ());

            if (lso ? at == "static" : at == "shared")
              fail << (lso ? "shared" : "static") << " build of " << *l
                   << " is not available";

            pt = lso ? static_cast<target*> (l->so) : l->a;

            if (pt == nullptr)
            {
              const target_type& type (
                lso ? libso::static_type : liba::static_type);

              pt = &search (
                prerequisite_key {&type, &p.dir, &p.name, &p.ext, &p.scope});
            }
          }

          build::match (a, *pt);
          t.prerequisite_targets.push_back (pt);
          continue;
        }

        if (out_root == nullptr)
        {
          // Which scope shall we use to resolve the root? Unlikely,
          // but possible, the prerequisite is from a different project
          // altogether. So we are going to use the target's project.
          //
          scope* rs (t.root_scope ());
          assert (rs != nullptr); // Shouldn't have matched.
          out_root = &rs->path ();
          src_root = &rs->src_path ();
        }

        prerequisite& cp (p);
        const target_type& o_type (
          group
          ? obj::static_type
          : (so ? objso::static_type : obja::static_type));

        // Come up with the obj*{} prerequisite. The c(xx){} prerequisite
        // directory can be relative (to the scope) or absolute. If it is
        // relative, then use it as is. If it is absolute, then translate
        // it to the corresponding directory under out_root. While the
        // c(xx){} directory is most likely under src_root, it is also
        // possible it is under out_root (e.g., generated source).
        //
        dir_path d;
        if (cp.dir.relative () || cp.dir.sub (*out_root))
          d = cp.dir;
        else
        {
          if (!cp.dir.sub (*src_root))
            fail << "out of project prerequisite " << cp <<
              info << "specify corresponding " << o_type.name << "{} "
                 << "target explicitly";

          d = *out_root / cp.dir.leaf (*src_root);
        }

        prerequisite& op (
          cp.scope.prerequisites.insert (
            o_type,
            move (d),
            cp.name,
            nullptr,
            cp.scope,
            trace).first);

        // Resolve this prerequisite to target.
        //
        target* ot (&search (op));

        // If we are cleaning, check that this target is in the same or
        // a subdirectory of ours.
        //
        // If it is not, then we are effectively leaving the prerequisites
        // half-rewritten (we only rewrite those that we should clean).
        // What will happen if, say, after clean we have update? Well,
        // update will come and finish the rewrite process (it will even
        // reuse op that we have created but then ignored). So all is good.
        //
        if (a.operation () == clean_id && !ot->dir.sub (t.dir))
        {
          // If we shouldn't clean obj{}, then it is fair to assume
          // we shouldn't clean cxx{} either (generated source will
          // be in the same directory as obj{} and if not, well, go
          // find yourself another build system).
          //
          continue; // Skip.
        }

        pt = ot;

        // If we have created the obj{} target group, pick one of its
        // members; the rest would be primarily concerned with it.
        //
        if (group)
        {
          obj& o (static_cast<obj&> (*ot));
          ot = so ? static_cast<target*> (o.so) : o.a;

          if (ot == nullptr)
          {
            const target_type& type (
              so ? objso::static_type : obja::static_type);

            ot = &search (
              prerequisite_key {&type, &o.dir, &o.name, &o.ext, nullptr});
          }
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
        for (prerequisite& p: reverse_iterate (group_prerequisites (*ot)))
        {
          // Ignore some known target types (fsdir, headers).
          //
          if (p.type.id == typeid (fsdir) ||
              p.type.id == typeid (h)     ||
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
               << " would be incompatible with existing target " << *ot <<
            info << "unknown existing prerequisite type " << p <<
            info << "specify corresponding obj{} target explicitly";
        }

        if (cp1 != nullptr)
        {
          build::match (a, *ot); // Now cp1 should be resolved.
          search (cp);           // Our own prerequisite, so this is ok.

          if (cp.target != cp1->target)
            fail << "synthesized target for prerequisite " << cp
                 << " would be incompatible with existing target " << *ot <<
              info << "existing prerequisite " << *cp1 << " does not "
                 << "match " << cp <<
              info << "specify corresponding " << o_type.name << "{} "
                 << "target explicitly";
        }
        else
        {
          // Note: add the source to the group, not the member.
          //
          pt->prerequisites.emplace_back (cp);
          build::match (a, *ot);
        }

        // Change the exe{} target's prerequisite from cxx{} to obj*{}.
        //
        pr = op;

        t.prerequisite_targets.push_back (ot);
      }

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id: return &perform_clean;
      default: return default_recipe; // Forward to prerequisites.
      }
    }

    target_state link::
    perform_update (action a, target& xt)
    {
      path_target& t (static_cast<path_target&> (xt));

      type tt (t.is_a<exe> ()
               ? type::exe
               : (t.is_a<liba> () ? type::liba : type::libso));

      bool so (tt == type::libso);

      if (!execute_prerequisites (a, t, t.mtime ()))
        return target_state::unchanged;

      // Translate paths to relative (to working directory) ones. This
      // results in easier to read diagnostics.
      //
      path relt (relative (t.path ()));
      vector<path> relo;

      scope& rs (*t.root_scope ()); // Shouldn't have matched if nullptr.
      vector<const char*> args;
      string storage1;

      if (tt == type::liba)
      {
        //@@ ranlib
        //
        args.push_back ("ar");
        args.push_back ("-rc");
        args.push_back (relt.string ().c_str ());
      }
      else
      {
        args.push_back (rs["config.cxx"].as<const string&> ().c_str ());

        append_options (args, t, "cxx.coptions");

        append_std (args, t, storage1);

        if (so)
          args.push_back ("-shared");

        args.push_back ("-o");
        args.push_back (relt.string ().c_str ());

        append_options (args, t, "cxx.loptions");
      }

      for (target* pt: t.prerequisite_targets)
      {
        path_target* ppt;

        if ((ppt = pt->is_a<obja> ()))
          ;
        else if ((ppt = pt->is_a<objso> ()))
          ;
        else if ((ppt = pt->is_a<liba> ()))
          ;
        else if ((ppt = pt->is_a<libso> ()))
          ;
        else
          continue;

        relo.push_back (relative (ppt->path ()));
        args.push_back (relo.back ().string ().c_str ());
      }

      if (tt != type::liba)
        append_options (args, t, "cxx.libs");

      args.push_back (nullptr);

      if (verb)
        print_process (args);
      else
        text << "ld " << t;

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
