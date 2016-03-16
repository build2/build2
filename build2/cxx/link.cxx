// file      : build2/cxx/link.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/link>

#include <cstdlib>  // exit()

#include <butl/path-map>
#include <butl/filesystem>

#include <build2/depdb>
#include <build2/scope>
#include <build2/variable>
#include <build2/algorithm>
#include <build2/diagnostics>
#include <build2/context>

#include <build2/bin/target>
#include <build2/cxx/target>

#include <build2/cxx/utility>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
  {
    using namespace bin;

    link::order link::
    link_order (target& t)
    {
      const char* var;

      switch (link_type (t))
      {
      case type::e:  var = "bin.exe.lib";   break;
      case type::a:  var = "bin.liba.lib";  break;
      case type::so: var = "bin.libso.lib"; break;
      }

      const auto& v (as<strings> (*t[var]));
      return v[0] == "shared"
        ? v.size () > 1 && v[1] == "static" ? order::so_a : order::so
        : v.size () > 1 && v[1] == "shared" ? order::a_so : order::a;
    }

    target& link::
    link_member (bin::lib& l, order lo)
    {
      bool lso (true);
      const string& at (as<string> (*l["bin.lib"])); // Available types.

      switch (lo)
      {
      case order::a:
      case order::a_so:
        lso = false; // Fall through.
      case order::so:
      case order::so_a:
        {
          if (lso ? at == "static" : at == "shared")
          {
            if (lo == order::a_so || lo == order::so_a)
              lso = !lso;
            else
              fail << (lso ? "shared" : "static") << " build of " << l
                   << " is not available";
          }
        }
      }

      target* r (lso ? static_cast<target*> (l.so) : l.a);

      if (r == nullptr)
        r = &search (lso ? libso::static_type : liba::static_type,
                     prerequisite_key {nullptr, l.key (), nullptr});

      return *r;
    }

    link::search_paths link::
    extract_library_paths (scope& bs)
    {
      search_paths r;
      scope& rs (*bs.root_scope ());

      // Extract user-supplied search paths (i.e., -L).
      //
      if (auto l = bs["cxx.loptions"])
      {
        const auto& v (as<strings> (*l));

        for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
        {
          // -L can either be in the "-Lfoo" or "-L foo" form.
          //
          dir_path d;
          if (*i == "-L")
          {
            if (++i == e)
              break; // Let the compiler complain.

            d = dir_path (*i);
          }
          else if (i->compare (0, 2, "-L") == 0)
            d = dir_path (*i, 2, string::npos);
          else
            continue;

          // Ignore relative paths. Or maybe we should warn?
          //
          if (!d.relative ())
            r.push_back (move (d));
        }
      }

      // Extract system search paths.
      //
      cstrings args;
      string std_storage;

      args.push_back (as<string> (*rs["config.cxx"]).c_str ());
      append_options (args, bs, "cxx.coptions");
      append_std (args, bs, std_storage);
      append_options (args, bs, "cxx.loptions");
      args.push_back ("-print-search-dirs");
      args.push_back (nullptr);

      if (verb >= 3)
        print_process (args);

      string l;
      try
      {
        process pr (args.data (), 0, -1); // Open pipe to stdout.
        ifdstream is (pr.in_ofd);

        while (!is.eof ())
        {
          string s;
          getline (is, s);

          if (is.fail () && !is.eof ())
            fail << "error reading C++ compiler -print-search-dirs output";

          if (s.compare (0, 12, "libraries: =") == 0)
          {
            l.assign (s, 12, string::npos);
            break;
          }
        }

        is.close (); // Don't block.

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }

      if (l.empty ())
        fail << "unable to extract C++ compiler system library paths";

      // Now the fun part: figuring out which delimiter is used.
      // Normally it is ':' but on Windows it is ';' (or can be;
      // who knows for sure). Also note that these paths are
      // absolute (or should be). So here is what we are going
      // to do: first look for ';'. If found, then that's the
      // delimiter. If not found, then there are two cases:
      // it is either a single Windows path or the delimiter
      // is ':'. To distinguish these two cases we check if
      // the path starts with a Windows drive.
      //
      char d (';');
      string::size_type e (l.find (d));

      if (e == string::npos &&
          (l.size () < 2 || l[0] == '/' || l[1] != ':'))
      {
        d = ':';
        e = l.find (d);
      }

      // Now chop it up. We already have the position of the
      // first delimiter (if any).
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

    target* link::
    search_library (search_paths_cache& spc, prerequisite& p)
    {
      tracer trace ("cxx::link::search_library");

      // First check the cache.
      //
      if (p.target != nullptr)
        return p.target;

      scope& rs (*p.scope.root_scope ());
      const string& sys (as<string> (*rs["cxx.target.system"]));

      bool l (p.is_a<lib> ());
      const string* ext (l ? nullptr : p.ext); // Only for liba/libso.

      // Then figure out what we need to search for.
      //

      // liba
      //
      path an;
      const string* ae;

      if (l || p.is_a<liba> ())
      {
        // We are trying to find a library in the search paths extracted from
        // the compiler. It would only be natural if we use the library
        // prefix/extension that correspond to this compiler's target.
        //
        an = path ("lib" + p.name);
        ae = ext == nullptr
          ? &extension_pool.find ("a")
          : ext;

        if (!ae->empty ())
        {
          an += '.';
          an += *ae;
        }
      }

      // libso
      //
      path sn;
      const string* se;

      if (l || p.is_a<libso> ())
      {
        sn = path ("lib" + p.name);

        if (ext == nullptr)
        {
          const char* e;
          if (sys == "darwin")
            e = "dylib";
          else
            e = "so";

          ext = &extension_pool.find (e);
        }

        se = ext;

        if (!se->empty ())
        {
          sn += '.';
          sn += *se;
        }
      }

      // Now search.
      //
      if (!spc)
        spc = extract_library_paths (p.scope);

      liba* a (nullptr);
      libso* s (nullptr);

      path f; // Reuse the buffer.
      const dir_path* pd;
      for (const dir_path& d: *spc)
      {
        timestamp mt;

        // liba
        //
        if (!an.empty ())
        {
          f = d;
          f /= an;

          if ((mt = file_mtime (f)) != timestamp_nonexistent)
          {
            // Enter the target. Note that because the search paths are
            // normalized, the result is automatically normalized as well.
            //
            a = &targets.insert<liba> (d, p.name, ae, trace);

            if (a->path ().empty ())
              a->path (move (f));

            a->mtime (mt);
          }
        }

        // libso
        //
        if (!sn.empty ())
        {
          f = d;
          f /= sn;

          if ((mt = file_mtime (f)) != timestamp_nonexistent)
          {
            s = &targets.insert<libso> (d, p.name, se, trace);

            if (s->path ().empty ())
              s->path (move (f));

            s->mtime (mt);
          }
        }

        if (a != nullptr || s != nullptr)
        {
          pd = &d;
          break;
        }
      }

      if (a == nullptr && s == nullptr)
        return nullptr;

      if (l)
      {
        // Enter the target group.
        //
        lib& l (targets.insert<lib> (*pd, p.name, p.ext, trace));

        // It should automatically link-up to the members we have found.
        //
        assert (l.a == a);
        assert (l.so == s);

        // Set the bin.lib variable to indicate what's available.
        //
        const char* bl (a != nullptr
                        ? (s != nullptr ? "both" : "static")
                        : "shared");
        l.assign ("bin.lib") = bl;

        p.target = &l;
      }
      else
        p.target = p.is_a<liba> () ? static_cast<target*> (a) : s;

      return p.target;
    }

    match_result link::
    match (action a, target& t, const string& hint) const
    {
      tracer trace ("cxx::link::match");

      // @@ TODO:
      //
      // - if path already assigned, verify extension?
      //
      // @@ Q:
      //
      // - if there is no .o, are we going to check if the one derived
      //   from target exist or can be built? A: No.
      //   What if there is a library. Probably ok if .a, not if .so.
      //   (i.e., a utility library).
      //

      type lt (link_type (t));

      // Scan prerequisites and see if we can work with what we've got.
      //
      bool seen_cxx (false), seen_c (false), seen_obj (false),
        seen_lib (false);

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (p.is_a<cxx> ())
        {
          seen_cxx = seen_cxx || true;
        }
        else if (p.is_a<c> ())
        {
          seen_c = seen_c || true;
        }
        else if (p.is_a<obja> ())
        {
          if (lt == type::so)
            fail << "shared library " << t << " prerequisite " << p
                 << " is static object";

          seen_obj = seen_obj || true;
        }
        else if (p.is_a<objso> () ||
                 p.is_a<obj> ())
        {
          seen_obj = seen_obj || true;
        }
        else if (p.is_a<liba> ()  ||
                 p.is_a<libso> () ||
                 p.is_a<lib> ())
        {
          seen_lib = seen_lib || true;
        }
      }

      // We will only chain a C source if there is also a C++ source or we
      // were explicitly told to.
      //
      if (seen_c && !seen_cxx && hint < "cxx")
      {
        l4 ([&]{trace << "c prerequisite(s) without c++ or hint";});
        return nullptr;
      }

      // If we have any prerequisite libraries (which also means that
      // we match), search/import and pre-match them to implement the
      // "library meta-information protocol". Don't do this if we are
      // called from the install rule just to check if we would match.
      //
      if (seen_lib && lt != type::e &&
          a.operation () != install_id && a.outer_operation () != install_id)
      {
        if (t.group != nullptr)
          t.group->prerequisite_targets.clear (); // lib{}'s

        search_paths_cache lib_paths; // Extract lazily.

        for (prerequisite_member p: group_prerequisite_members (a, t))
        {
          if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libso> ())
          {
            target* pt (nullptr);

            // Handle imported libraries.
            //
            if (p.proj () != nullptr)
              pt = search_library (lib_paths, p.prerequisite);

            if (pt == nullptr)
            {
              pt = &p.search ();
              match_only (a, *pt);
            }

            // If the prerequisite came from the lib{} group, then also
            // add it to lib's prerequisite_targets.
            //
            if (!p.prerequisite.belongs (t))
              t.group->prerequisite_targets.push_back (pt);

            t.prerequisite_targets.push_back (pt);
          }
        }
      }

      return seen_cxx || seen_c || seen_obj || seen_lib ? &t : nullptr;
    }

    recipe link::
    apply (action a, target& xt, const match_result&) const
    {
      tracer trace ("cxx::link::apply");

      path_target& t (static_cast<path_target&> (xt));

      scope& rs (t.root_scope ());
      const string& sys (as<string> (*rs["cxx.target.system"]));

      type lt (link_type (t));
      bool so (lt == type::so);
      order lo (link_order (t));

      // Derive file name from target name.
      //
      if (t.path ().empty ())
      {
        switch (lt)
        {
        case type::e:
          {
            const char* e;
            if (sys == "mingw32")
              e = "exe";
            else
              e = "";

            t.derive_path (e);
            break;
          }
        case type::a:
        case type::so:
          {
            auto l (t["bin.libprefix"]);
            const char* p (l ? as<string> (*l).c_str () : "lib");

            const char* e;
            if (lt == type::a)
            {
              e = "a";
            }
            else
            {
              if (sys == "darwin")
                e = "dylib";
              else
                e = "so";
            }

            t.derive_path (e, p);
            break;
          }
        }
      }

      t.prerequisite_targets.clear (); // See lib pre-match in match() above.

      // Inject dependency on the output directory.
      //
      inject_parent_fsdir (a, t);

      search_paths_cache lib_paths; // Extract lazily.

      // Process prerequisites: do rule chaining for C and C++ source
      // files as well as search and match.
      //
      // When cleaning, ignore prerequisites that are not in the same
      // or a subdirectory of our project root.
      //
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        bool group (!p.prerequisite.belongs (t)); // Group's prerequisite.
        target* pt (nullptr);

        if (!p.is_a<c> () && !p.is_a<cxx> ())
        {
          // Handle imported libraries.
          //
          if (p.proj () != nullptr)
            pt = search_library (lib_paths, p.prerequisite);

          // The rest is the same basic logic as in search_and_match().
          //
          if (pt == nullptr)
            pt = &p.search ();

          if (a.operation () == clean_id && !pt->dir.sub (rs.out_path ()))
            continue; // Skip.

          // If this is the obj{} or lib{} target group, then pick the
          // appropriate member and make sure it is searched and matched.
          //
          if (obj* o = pt->is_a<obj> ())
          {
            pt = so ? static_cast<target*> (o->so) : o->a;

            if (pt == nullptr)
              pt = &search (so ? objso::static_type : obja::static_type,
                            p.key ());
          }
          else if (lib* l = pt->is_a<lib> ())
          {
            pt = &link_member (*l, lo);
          }

          build2::match (a, *pt);
          t.prerequisite_targets.push_back (pt);
          continue;
        }

        // Which scope shall we use to resolve the root? Unlikely, but
        // possible, the prerequisite is from a different project
        // altogether. So we are going to use the target's project.
        //

        const prerequisite_key& cp (p.key ()); // c(xx){} prerequisite key.
        const target_type& o_type (
          group
          ? obj::static_type
          : (so ? objso::static_type : obja::static_type));

        // Come up with the obj*{} target. The c(xx){} prerequisite
        // directory can be relative (to the scope) or absolute. If it is
        // relative, then use it as is. If it is absolute, then translate
        // it to the corresponding directory under out_root. While the
        // c(xx){} directory is most likely under src_root, it is also
        // possible it is under out_root (e.g., generated source).
        //
        dir_path d;
        {
          const dir_path& cpd (*cp.tk.dir);

          if (cpd.relative () || cpd.sub (rs.out_path ()))
            d = cpd;
          else
          {
            if (!cpd.sub (rs.src_path ()))
              fail << "out of project prerequisite " << cp <<
                info << "specify corresponding " << o_type.name << "{} "
                   << "target explicitly";

            d = rs.out_path () / cpd.leaf (rs.src_path ());
          }
        }

        target& ot (search (o_type, d, *cp.tk.name, nullptr, cp.scope));

        // If we are cleaning, check that this target is in the same or
        // a subdirectory of our project root.
        //
        if (a.operation () == clean_id && !ot.dir.sub (rs.out_path ()))
        {
          // If we shouldn't clean obj{}, then it is fair to assume
          // we shouldn't clean cxx{} either (generated source will
          // be in the same directory as obj{} and if not, well, go
          // find yourself another build system ;-)).
          //
          continue; // Skip.
        }

        // If we have created the obj{} target group, pick one of its
        // members; the rest would be primarily concerned with it.
        //
        if (group)
        {
          obj& o (static_cast<obj&> (ot));
          pt = so ? static_cast<target*> (o.so) : o.a;

          if (pt == nullptr)
            pt = &search (so ? objso::static_type : obja::static_type,
                          o.dir, o.name, o.ext, nullptr);
        }
        else
          pt = &ot;

        // If this obj*{} target already exists, then it needs to be
        // "compatible" with what we are doing here.
        //
        // This gets a bit tricky. We need to make sure the source files
        // are the same which we can only do by comparing the targets to
        // which they resolve. But we cannot search the ot's prerequisites
        // -- only the rule that matches can. Note, however, that if all
        // this works out, then our next step is to match the obj*{}
        // target. If things don't work out, then we fail, in which case
        // searching and matching speculatively doesn't really hurt.
        //
        bool found (false);
        for (prerequisite_member p1:
               reverse_group_prerequisite_members (a, *pt))
        {
          // Ignore some known target types (fsdir, headers, libraries).
          //
          if (p1.is_a<fsdir> () ||
              p1.is_a<h> ()     ||
              (p.is_a<cxx> () && (p1.is_a<hxx> () ||
                                  p1.is_a<ixx> () ||
                                  p1.is_a<txx> ())) ||
              p1.is_a<lib> ()  ||
              p1.is_a<liba> () ||
              p1.is_a<libso> ())
          {
            continue;
          }

          if (!p1.is_a<cxx> ())
            fail << "synthesized target for prerequisite " << cp
                 << " would be incompatible with existing target " << *pt <<
              info << "unexpected existing prerequisite type " << p1 <<
              info << "specify corresponding obj{} target explicitly";

          if (!found)
          {
            build2::match (a, *pt); // Now p1 should be resolved.

            // Searching our own prerequisite is ok.
            //
            if (&p.search () != &p1.search ())
              fail << "synthesized target for prerequisite " << cp << " would "
                   << "be incompatible with existing target " << *pt <<
                info << "existing prerequisite " << p1 << " does not match "
                   << cp <<
                info << "specify corresponding " << o_type.name << "{} target "
                   << "explicitly";

            found = true;
            // Check the rest of the prerequisites.
          }
        }

        if (!found)
        {
          // Note: add the source to the group, not the member.
          //
          ot.prerequisites.emplace_back (p.as_prerequisite (trace));

          // Add our lib*{} prerequisites to the object file (see
          // cxx.export.poptions above for details). Note: no need
          // to go into group members.
          //
          // Initially, we were only adding imported libraries, but
          // there is a problem with this approach: the non-imported
          // library might depend on the imported one(s) which we will
          // never "see" unless we start with this library.
          //
          for (prerequisite& p: group_prerequisites (t))
          {
            if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libso> ())
              ot.prerequisites.emplace_back (p);
          }

          build2::match (a, *pt);
        }

        t.prerequisite_targets.push_back (pt);
      }

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id: return &perform_clean_depdb;
      default: return noop_recipe; // Configure update.
      }
    }

    static void
    append_rpath_link (strings& args, libso& t)
    {
      for (target* pt: t.prerequisite_targets)
      {
        if (libso* ls = pt->is_a<libso> ())
        {
          args.push_back ("-Wl,-rpath-link," +
                          ls->path ().directory ().string ());
          append_rpath_link (args, *ls);
        }
      }
    }

    target_state link::
    perform_update (action a, target& xt)
    {
      tracer trace ("cxx::link::perform_update");

      path_target& t (static_cast<path_target&> (xt));

      type lt (link_type (t));
      bool so (lt == type::so);

      // Update prerequisites.
      //
      bool up (execute_prerequisites (a, t, t.mtime ()));

      scope& rs (t.root_scope ());
      const string& sys (as<string> (*rs["cxx.target.system"]));

      // Check/update the dependency database.
      //
      depdb dd (t.path () + ".d");

      // First should come the rule name/version.
      //
      if (dd.expect ("cxx.link 1") != nullptr)
        l4 ([&]{trace << "rule mismatch forcing update of " << t;});

      lookup<const value> ranlib;

      // Then the linker checksum (ar/ranlib or C++ compiler).
      //
      if (lt == type::a)
      {
        ranlib = rs["config.bin.ranlib"];

        if (ranlib->empty ()) // @@ TMP until proper NULL support.
          ranlib = lookup<const value> ();

        const char* rl (
          ranlib
          ? as<string> (*rs["bin.ranlib.checksum"]).c_str ()
          : "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        if (dd.expect (as<string> (*rs["bin.ar.checksum"])) != nullptr)
          l4 ([&]{trace << "ar mismatch forcing update of " << t;});

        if (dd.expect (rl) != nullptr)
          l4 ([&]{trace << "ranlib mismatch forcing update of " << t;});
      }
      else
      {
        if (dd.expect (as<string> (*rs["cxx.checksum"])) != nullptr)
          l4 ([&]{trace << "compiler mismatch forcing update of " << t;});
      }

      // Start building the command line. While we don't yet know whether we
      // will really need it, we need to hash it to find out. So the options
      // are to either replicate the exact process twice, first for hashing
      // then for building or to go ahead and start building and hash the
      // result. The first approach is probably more efficient while the
      // second is simpler. Let's got with the simpler for now (actually it's
      // kind of a hybrid).
      //
      cstrings args {nullptr}; // Reserve for config.bin.ar/config.cxx.

      // Storage.
      //
      string std;
      string soname1, soname2;
      strings sargs;

      if (lt == type::a)
      {
        // If the user asked for ranlib, don't try to do its function with -s.
        // Some ar implementations (e.g., the LLVM one) doesn't support
        // leading '-'.
        //
        args.push_back (ranlib ? "rc" : "rcs");
      }
      else
      {
        append_options (args, t, "cxx.coptions");
        append_std (args, t, std);

        if (so)
        {
          if (sys == "darwin")
            args.push_back ("-dynamiclib");
          else
            args.push_back ("-shared");
        }

        // Set soname.
        //
        if (so)
        {
          const string& leaf (t.path ().leaf ().string ());

          if (sys == "darwin")
          {
            // With Mac OS 10.5 (Leopard) Apple finally caved in and gave us
            // a way to emulate vanilla -rpath.
            //
            // It may seem natural to do something different on update for
            // install. However, if we don't make it @rpath, then the user
            // won't be able to use config.bin.rpath for installed libraries.
            //
            soname1 = "-install_name";
            soname2 = "@rpath/" + leaf;
          }
          else
            soname1 = "-Wl,-soname," + leaf;

          if (!soname1.empty ())
            args.push_back (soname1.c_str ());

          if (!soname2.empty ())
            args.push_back (soname2.c_str ());
        }

        // Add rpaths. First the ones specified by the user so that they take
        // precedence.
        //
        if (auto l = t["bin.rpath"])
          for (const string& p: as<strings> (*l))
            sargs.push_back ("-Wl,-rpath," + p);

        // Then the paths of the shared libraries we are linking to. Unless
        // this is update for install, in which case we have to do something
        // different.
        //
        for (target* pt: t.prerequisite_targets)
        {
          if (libso* ls = pt->is_a<libso> ())
          {
            if (a.outer_operation () != install_id)
            {
              sargs.push_back ("-Wl,-rpath," +
                               ls->path ().directory ().string ());
            }
            // Use -rpath-link on targets that support it (Linux, FreeBSD).
            // Since with this option the paths are not stored in the library,
            // we have to do this recursively (in fact, we don't really need
            // it for top-level libraries).
            //
            else if (sys != "darwin")
              append_rpath_link (sargs, *ls);
          }
        }
      }

      // All the options should now be in. Hash them and compare with the db.
      //
      {
        sha256 cs;

        for (size_t i (1); i != args.size (); ++i)
          cs.append (args[i]);

        for (size_t i (0); i != sargs.size (); ++i)
          cs.append (sargs[i]);

        if (lt != type::a)
          hash_options (cs, t, "cxx.loptions");

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "options mismatch forcing update of " << t;});
      }

      // Finally, hash and compare the list of input files.
      //
      // Should we capture actual files or their checksum? The only good
      // reason for capturing actual files is diagnostics: we will be able
      // to pinpoint exactly what is causing the update. On the other hand,
      // the checksum is faster and simpler.
      //
      {
        sha256 cs;

        for (target* pt: t.prerequisite_targets)
        {
          path_target* ppt;

          if ((ppt = pt->is_a<obja> ())  ||
              (ppt = pt->is_a<objso> ()) ||
              (ppt = pt->is_a<liba> ())  ||
              (ppt = pt->is_a<libso> ()))
          {
            cs.append (ppt->path ().string ());
          }
        }

        // Treat them as inputs, not options.
        //
        if (lt != type::a)
          hash_options (cs, t, "cxx.libs");

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "file set mismatch forcing update of " << t;});
      }

      // If any of the above checks resulted in a mismatch (different linker,
      // options, or input file set), or if the database is newer than the
      // target (interrupted update) then force the target update.
      //
      if (dd.writing () || dd.mtime () > t.mtime ())
        up = true;

      dd.close ();

      // If nothing changed, then we are done.
      //
      if (!up)
        return target_state::unchanged;

      // Ok, so we are updating. Finish building the command line.
      //

      // Translate paths to relative (to working directory) ones. This results
      // in easier to read diagnostics.
      //
      path relt (relative (t.path ()));

      if (lt == type::a)
      {
        args[0] = as<string> (*rs["config.bin.ar"]).c_str ();
        args.push_back (relt.string ().c_str ());
      }
      else
      {
        args[0] = as<string> (*rs["config.cxx"]).c_str ();
        args.push_back ("-o");
        args.push_back (relt.string ().c_str ());
      }

      size_t oend (sargs.size ()); // Note the end of options in sargs.

      for (target* pt: t.prerequisite_targets)
      {
        path_target* ppt;

        if ((ppt = pt->is_a<obja> ())  ||
            (ppt = pt->is_a<objso> ()) ||
            (ppt = pt->is_a<liba> ())  ||
            (ppt = pt->is_a<libso> ()))
        {
          sargs.push_back (relative (ppt->path ()).string ()); // string()&&
        }
      }

      // Copy sargs to args. Why not do it as we go along pusing into sargs?
      // Because of potential realocations.
      //
      for (size_t i (0); i != sargs.size (); ++i)
      {
        if (lt != type::a && i == oend)
          append_options (args, t, "cxx.loptions");

        args.push_back (sargs[i].c_str ());
      }

      if (lt != type::a)
        append_options (args, t, "cxx.libs");

      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "ld " << t;

      try
      {
        process pr (args.data ());

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

      if (ranlib)
      {
        const char* args[] = {
          as<string> (*ranlib).c_str (), relt.string ().c_str (), nullptr};

        if (verb >= 2)
          print_process (args);

        try
        {
          process pr (args);

          if (!pr.wait ())
            throw failed ();
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << args[0] << ": " << e.what ();

          if (e.child ())
            exit (1);

          throw failed ();
        }
      }

      // Should we go to the filesystem and get the new mtime? We know the
      // file has been modified, so instead just use the current clock time.
      // It has the advantage of having the subseconds precision.
      //
      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    link link::instance;
  }
}
