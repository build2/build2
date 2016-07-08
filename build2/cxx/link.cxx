// file      : build2/cxx/link.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/link>

#include <errno.h> // E*

#include <set>
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

      const auto& v (cast<strings> (t[var]));
      return v[0] == "shared"
        ? v.size () > 1 && v[1] == "static" ? order::so_a : order::so
        : v.size () > 1 && v[1] == "shared" ? order::a_so : order::a;
    }

    target& link::
    link_member (bin::lib& l, order lo)
    {
      bool lso (true);
      const string& at (cast<string> (l["bin.lib"])); // Available types.

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

    // Extract system library search paths from GCC or compatible (Clang,
    // Intel C++) using the -print-search-dirs option.
    //
    static void
    gcc_library_search_paths (scope& bs, const string& cid, dir_paths& r)
    {
      scope& rs (*bs.root_scope ());

      cstrings args;
      string std_storage;

      args.push_back (cast<path> (rs["config.cxx"]).string ().c_str ());
      append_options (args, bs, "cxx.coptions");
      append_std (args, rs, cid, bs, std_storage);
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
    }

    // Extract system library search paths from MSVC. The linker doesn't seem
    // to have any built-in paths and all of them are passed via the LIB
    // environment variable.
    //
    static void
    msvc_library_search_paths (scope&, const string&, dir_paths&)
    {
      // @@ VC: how are we going to do this? E.g., cl-14 does this internally.
      //    Maybe that cld.c hack, seems to be passing stuff from INCLUDE..?
    }

    dir_paths link::
    extract_library_paths (scope& bs)
    {
      dir_paths r;
      scope& rs (*bs.root_scope ());
      const string& cid (cast<string> (rs["cxx.id"]));

      // Extract user-supplied search paths (i.e., -L, /LIBPATH).
      //
      if (auto l = bs["cxx.loptions"])
      {
        const auto& v (cast<strings> (l));

        for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
        {
          dir_path d;

          if (cid == "msvc")
          {
            // /LIBPATH:<dir>
            //
            if (i->compare (0, 9, "/LIBPATH:") == 0 ||
                i->compare (0, 9, "-LIBPATH:") == 0)
              d = dir_path (*i, 9, string::npos);
            else
              continue;
          }
          else
          {
            // -L can either be in the "-L<dir>" or "-L <dir>" form.
            //
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
          }

          // Ignore relative paths. Or maybe we should warn?
          //
          if (!d.relative ())
            r.push_back (move (d));
        }
      }

      if (cid == "msvc")
        msvc_library_search_paths (bs, cid, r);
      else
        gcc_library_search_paths (bs, cid, r);

      return r;
    }

    target* link::
    search_library (optional<dir_paths>& spc, prerequisite& p)
    {
      tracer trace ("cxx::link::search_library");

      // First check the cache.
      //
      if (p.target != nullptr)
        return p.target;

      scope& rs (*p.scope.root_scope ());
      const string& cid (cast<string> (rs["cxx.id"]));
      const string& tsys (cast<string> (rs["cxx.target.system"]));

      bool l (p.is_a<lib> ());
      const string* ext (l ? nullptr : p.ext); // Only for liba/libso.

      // Then figure out what we need to search for.
      //

      // liba
      //
      path an;
      const string* ae (nullptr);

      if (l || p.is_a<liba> ())
      {
        // We are trying to find a library in the search paths extracted from
        // the compiler. It would only be natural if we used the library
        // prefix/extension that correspond to this compiler and/or its
        // target.
        //
        const char* e ("");

        if (cid == "msvc")
        {
          an = path (p.name);
          e = "lib";
        }
        else
        {
          an = path ("lib" + p.name);
          e = "a";
        }

        ae = ext == nullptr
          ? &extension_pool.find (e)
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
      const string* se (nullptr);

      if (l || p.is_a<libso> ())
      {
        const char* e ("");

        if (cid == "msvc")
        {
          // @@ VC TODO: still .lib, right?
          //
        }
        else
        {
          sn = path ("lib" + p.name);

          if      (tsys == "darwin")  e = "dylib";
          else if (tsys == "mingw32") e = "dll.a"; // See search code below.
          else                        e = "so";
        }

        se = ext == nullptr
          ? &extension_pool.find (e)
          : ext;

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
            // Note that this target is outside any project which we treat
            // as out trees.
            //
            a = &targets.insert<liba> (d, dir_path (), p.name, ae, trace);

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
          mt = file_mtime (f);

          if (tsys == "mingw32")
          {
            // Above we searched for the import library (.dll.a) but if it's
            // not found, then we also search for the .dll (unless the
            // extension was specified explicitly) since we can link to it
            // directly. Note also that the resulting libso{} would end up
            // being the .dll.
            //
            if (mt == timestamp_nonexistent && ext == nullptr)
            {
              se = &extension_pool.find ("dll");
              f = f.base (); // Remove .a from .dll.a.
              mt = file_mtime (f);
            }
          }

          if (mt != timestamp_nonexistent)
          {
            s = &targets.insert<libso> (d, dir_path (), p.name, se, trace);

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
        lib& l (targets.insert<lib> (*pd, dir_path (), p.name, p.ext, trace));

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
        l4 ([&]{trace << "C prerequisite(s) without C++ or hint";});
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

        optional<dir_paths> lib_paths; // Extract lazily.

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

      scope& bs (t.base_scope ());
      scope& rs (*bs.root_scope ());
      const string& tsys (cast<string> (rs["cxx.target.system"]));
      const string& tclass (cast<string> (rs["cxx.target.class"]));

      type lt (link_type (t));
      order lo (link_order (t));

      // Some targets have all object files the same.
      //
      bool so (lt == type::so);
      bool oso (so && tclass != "macosx" && tclass != "windows");

      // Derive file name from target name.
      //
      if (t.path ().empty ())
      {
        const char* p (nullptr);
        const char* e (nullptr);

        switch (lt)
        {
        case type::e:
          {
            if (tclass == "windows")
              e = "exe";
            else
              e = "";

            break;
          }
        case type::a:
          {
            // To be anally precise, let's use the ar id to decide how to name
            // the library in case, for example, someone wants to archive
            // VC-compiled object files with MinGW ar.
            //
            if (cast<string> (rs["bin.ar.id"]) == "msvc")
            {
              e = "lib";
            }
            else
            {
              p = "lib";
              e = "a";
            }

            if (auto l = t["bin.libprefix"])
              p = cast<string> (l).c_str ();

            break;
          }
        case type::so:
          {
            //@@ VC: DLL name.

            if (tclass == "macosx")
            {
              p = "lib";
              e = "dylib";
            }
            else if (tclass == "windows")
            {
              // On Windows libso{} is an ad hoc group. The libso{} itself is
              // the import library and we add dll{} as a member (see below).
              // While at first it may seem strange that libso{} is the import
              // library and not the DLL, if you meditate on it, you will see
              // it makes a lot of sense: our main task here is building and
              // for that we need the import library, not the DLL.
              //
              if (tsys == "mingw32")
              {
                p = "lib";
                e = "dll.a";
              }
              else
              {
                // Usually on Windows the import library is called the same as
                // the DLL but with the .lib extension. Which means it clashes
                // with the static library. Instead of decorating the static
                // library name with ugly suffixes (as is customary), let's
                // use the MinGW approach (one must admit it's quite elegant)
                // and call it .dll.lib.
                //
                e = "dll.lib";
              }
            }
            else
            {
              p = "lib";
              e = "so";
            }

            if (auto l = t["bin.libprefix"])
              p = cast<string> (l).c_str ();

            break;
          }
        }

        t.derive_path (e, p);
      }

      // On Windows add the DLL as an ad hoc group member.
      //
      if (so && tclass == "windows")
      {
        file* dll (nullptr);

        // Registered by cxx module's init().
        //
        const target_type& tt (*bs.find_target_type ("dll"));

        if (t.member != nullptr) // Might already be there.
        {
          assert (t.member->type () == tt);
          dll = static_cast<file*> (t.member);
        }
        else
        {
          t.member = dll = static_cast<file*> (
            &search (tt, t.dir, t.out, t.name, nullptr, nullptr));
        }

        if (dll->path ().empty ())
          dll->derive_path ("dll", tsys == "mingw32" ? "lib" : nullptr);

        dll->recipe (a, group_recipe);
      }

      t.prerequisite_targets.clear (); // See lib pre-match in match() above.

      // Inject dependency on the output directory.
      //
      inject_parent_fsdir (a, t);

      optional<dir_paths> lib_paths; // Extract lazily.

      // Process prerequisites: do rule chaining for C and C++ source
      // files as well as search and match.
      //
      // When cleaning, ignore prerequisites that are not in the same
      // or a subdirectory of our project root.
      //
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
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
            pt = oso ? static_cast<target*> (o->so) : o->a;

            if (pt == nullptr)
              pt = &search (oso ? objso::static_type : obja::static_type,
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

        // @@ Why are we creating the obj{} group if the source came from a
        //    group?
        //
        bool group (!p.prerequisite.belongs (t)); // Group's prerequisite.

        const prerequisite_key& cp (p.key ()); // c(xx){} prerequisite key.
        const target_type& otype (
          group
          ? obj::static_type
          : (oso ? objso::static_type : obja::static_type));

        // Come up with the obj*{} target. The c(xx){} prerequisite directory
        // can be relative (to the scope) or absolute. If it is relative, then
        // use it as is. If absolute, then translate it to the corresponding
        // directory under out_root. While the c(xx){} directory is most
        // likely under src_root, it is also possible it is under out_root
        // (e.g., generated source).
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
                info << "specify corresponding " << otype.name << "{} "
                   << "target explicitly";

            d = rs.out_path () / cpd.leaf (rs.src_path ());
          }
        }

        // obj*{} is always in the out tree.
        //
        target& ot (
          search (otype, d, dir_path (), *cp.tk.name, nullptr, cp.scope));

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
          pt = oso ? static_cast<target*> (o.so) : o.a;

          if (pt == nullptr)
            pt = &search (oso ? objso::static_type : obja::static_type,
                          o.dir, o.out, o.name, o.ext, nullptr);
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
                info << "specify corresponding " << otype.name << "{} target "
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
      case perform_clean_id: return &perform_clean;
      default: return noop_recipe; // Configure update.
      }
    }

    // Recursively append/hash prerequisite libraries of a static library.
    //
    static void
    append_libraries (strings& args, liba& a)
    {
      for (target* pt: a.prerequisite_targets)
      {
        if (liba* pa = pt->is_a<liba> ())
        {
          args.push_back (relative (pa->path ()).string ()); // string()&&
          append_libraries (args, *pa);
        }
        else if (libso* ps = pt->is_a<libso> ())
          args.push_back (relative (ps->path ()).string ()); // string()&&
      }
    }

    static void
    hash_libraries (sha256& cs, liba& a)
    {
      for (target* pt: a.prerequisite_targets)
      {
        if (liba* pa = pt->is_a<liba> ())
        {
          cs.append (pa->path ().string ());
          hash_libraries (cs, *pa);
        }
        else if (libso* ps = pt->is_a<libso> ())
          cs.append (ps->path ().string ());
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

    // Provide limited emulation of the rpath functionality on Windows using a
    // manifest and a side-by-side assembly. In a nutshell, the idea is to
    // create an assembly with links to all the prerequisite DLLs.
    //
    // The scratch argument should be true if the DLL set has changed and we
    // need to regenerate everything from scratch. Otherwise, we try to avoid
    // unnecessary work by comparing the DLL timestamps against the assembly
    // manifest file.
    //
    // If the manifest argument is false, then don't generate the target
    // manifest (i.e., it will be embedded).
    //
    // Note that currently our assemblies contain all the DLLs that the
    // executable depends on, recursively. The alternative approach could be
    // to also create assemblies for DLLs. This appears to be possible (but we
    // will have to use the resource ID 2 for such a manifest). And it will
    // probably be necessary for DLLs that are loaded dynamically with
    // LoadLibrary(). The tricky part is how such nested assemblies will be
    // found. Since we are effectively (from the loader's point of view)
    // copying the DLLs, we will also have to copy their assemblies (because
    // the loader looks for them in the same directory as the DLL).  It's not
    // clear how well such nested assemblies are supported.
    //
    static timestamp
    timestamp_dlls (target&);

    static void
    collect_dlls (set<file*>&, target&);

    static void
    emulate_rpath_windows (file& t, bool scratch, bool manifest)
    {
      // Assembly paths and name.
      //
      dir_path ad (path_cast<dir_path> (t.path () + ".dlls"));
      string an (ad.leaf ().string ());
      path am (ad / path (an + ".manifest"));

      // First check if we actually need to do anything. Since most of the
      // time we won't, we don't want to combine it with the collect_dlls()
      // call below which allocates memory, etc.
      //
      if (!scratch)
      {
        // The corner case here is when timestamp_dlls() returns nonexistent
        // signalling that there aren't any DLLs but the assembly manifest
        // file exists. This, however, can only happen if we somehow managed
        // to transition from the "have DLLs" state to "no DLLs" without going
        // through the from scratch update. And this shouldn't happen (famous
        // last words before a core dump).
        //
        if (timestamp_dlls (t) <= file_mtime (am))
          return;
      }

      scope& rs (t.root_scope ());

      // Next collect the set of DLLs that will be in our assembly. We need to
      // do this recursively which means we may end up with duplicates. Also,
      // it is possible that there will (no longer) be any DLLs which means we
      // just need to clean things up.
      //
      set<file*> dlls;
      collect_dlls (dlls, t);
      bool empty (dlls.empty ());

      // Target manifest.
      //
      path tm;
      if (manifest)
        tm = t.path () + ".manifest";

      // Clean the assembly directory and make sure it exists. Maybe it would
      // have been faster to overwrite the existing manifest rather than
      // removing the old one and creating a new one. But this is definitely
      // simpler.
      //
      {
        rmdir_status s (build2::rmdir_r (ad, empty, 3));

        // What if there is a user-defined manifest in the src directory? We
        // would just overwrite it if src == out. While we could add a comment
        // with some signature that can be used to detect an auto-generated
        // manifest, we can also use the presence of the assembly directory as
        // such a marker.
        //
        // @@ And what can we do instead? One idea is for the user to call it
        // something else and we merge the two. Perhaps the link rule could
        // have support for manifests (i.e., manifest will be one of the
        // prerequisites). A similar problem is with embedded vs standalone
        // manifests (embedded preferred starting from Vista). I guess if we
        // support embedding manifests, then we can also merge them.
        //
        if (manifest &&
            s == rmdir_status::not_exist &&
            rs.src_path () == rs.out_path () &&
            file_exists (tm))
        {
          fail << tm << " looks like a custom manifest" <<
            info << "remove it manually if that's not the case";
        }

        if (empty)
        {
          if (manifest)
            rmfile (tm, 3);

          return;
        }

        if (s == rmdir_status::not_exist)
          mkdir (ad, 3);
      }

      // Translate the compiler target CPU value to the processorArchitecture
      // attribute value.
      //
      const string& tcpu (cast<string> (rs["cxx.target.cpu"]));

      const char* pa (tcpu == "i386" || tcpu == "i686"  ? "x86"   :
                      tcpu == "x86_64"                  ? "amd64" :
                      nullptr);

      if (pa == nullptr)
        fail << "unable to translate CPU " << tcpu << " to manifest "
             << "processor architecture";

      if (verb >= 3)
        text << "cat >" << am;

      try
      {
        ofstream ofs;
        ofs.exceptions (ofstream::failbit | ofstream::badbit);
        ofs.open (am.string ());

        ofs << "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
            << "<assembly xmlns='urn:schemas-microsoft-com:asm.v1'\n"
            << "          manifestVersion='1.0'>\n"
            << "  <assemblyIdentity name='" << an << "'\n"
            << "                    type='win32'\n"
            << "                    processorArchitecture='" << pa << "'\n"
            << "                    version='0.0.0.0'/>\n";

        scope& as (*rs.weak_scope ()); // Amalgamation scope.

        for (file* dt: dlls)
        {
          const path& dp (dt->path ()); // DLL path.
          const path dn (dp.leaf ());   // DLL name.
          const path lp (ad / dn);      // Link path.

          auto print = [&dp, &lp] (const char* cmd)
          {
            if (verb >= 3)
              text << cmd << ' ' << dp << ' ' << lp;
          };

          // First we try to create a symlink. If that fails (e.g., "Windows
          // happens"), then we resort to hard links. If that doesn't work
          // out either (e.g., not on the same filesystem), then we fall back
          // to copies. So things are going to get a bit nested.
          //
          try
          {
            // For the symlink use a relative target path if both paths are
            // part of the same amalgamation. This way if the amalgamation is
            // moved as a whole, the links will remain valid.
            //
            if (dp.sub (as.out_path ()))
              mksymlink (dp.relative (ad), lp);
            else
              mksymlink (dp, lp);

            print ("ln -s");
          }
          catch (const system_error& e)
          {
            int c (e.code ().value ());

            if (c != EPERM && c != ENOSYS)
            {
              print ("ln -s");
              fail << "unable to create symlink " << lp << ": " << e.what ();
            }

            try
            {
              mkhardlink (dp, lp);
              print ("ln");
            }
            catch (const system_error& e)
            {
              int c (e.code ().value ());

              if (c != EPERM && c != ENOSYS)
              {
                print ("ln");
                fail << "unable to create hard link " << lp << ": "
                     << e.what ();
              }

              try
              {
                cpfile (dp, lp);
                print ("cp");
              }
              catch (const system_error& e)
              {
                print ("cp");
                fail << "unable to create copy " << lp << ": " << e.what ();
              }
            }
          }

          ofs << "  <file name='" << dn.string () << "'/>\n";
        }

        ofs << "</assembly>\n";
      }
      catch (const ofstream::failure&)
      {
        fail << "unable to write to " << am;
      }

      // Create the manifest if requested.
      //
      if (!manifest)
        return;

      if (verb >= 3)
        text << "cat >" << tm;

      try
      {
        ofstream ofs;
        ofs.exceptions (ofstream::failbit | ofstream::badbit);
        ofs.open (tm.string (), ofstream::out | ofstream::trunc);

        ofs << "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
            << "<!-- Note: auto-generated, do not edit. -->\n"
            << "<assembly xmlns='urn:schemas-microsoft-com:asm.v1'\n"
            << "          manifestVersion='1.0'>\n"
            << "  <assemblyIdentity name='" << t.path ().leaf () << "'\n"
            << "                    type='win32'\n"
            << "                    processorArchitecture='" << pa << "'\n"
            << "                    version='0.0.0.0'/>\n"
            << "  <dependency>\n"
            << "    <dependentAssembly>\n"
            << "      <assemblyIdentity name='" << an << "'\n"
            << "                        type='win32'\n"
            << "                        processorArchitecture='" << pa << "'\n"
            << "                        language='*'\n"
            << "                        version='0.0.0.0'/>\n"
            << "    </dependentAssembly>\n"
            << "  </dependency>\n"
            << "</assembly>\n";
      }
      catch (const ofstream::failure&)
      {
        fail << "unable to write to " << tm;
      }
    }

    // Return the greatest (newest) timestamp of all the DLLs that we will be
    // adding to the assembly or timestamp_nonexistent if there aren't any.
    //
    static timestamp
    timestamp_dlls (target& t)
    {
      timestamp r (timestamp_nonexistent);

      for (target* pt: t.prerequisite_targets)
      {
        if (libso* ls = pt->is_a<libso> ())
        {
          // This can be an installed library in which case we will have just
          // the import stub but may also have just the DLL. For now we don't
          // bother with installed libraries.
          //
          if (ls->member == nullptr)
            continue;

          file& dll (static_cast<file&> (*ls->member));

          // What if the DLL is in the same directory as the executable, will
          // it still be found even if there is an assembly? On the other
          // hand, handling it as any other won't hurt us much.
          //
          timestamp t;

          if ((t = dll.mtime ()) > r)
            r = t;

          if ((t = timestamp_dlls (*ls)) > r)
            r = t;
        }
      }

      return r;
    }

    static void
    collect_dlls (set<file*>& s, target& t)
    {
      for (target* pt: t.prerequisite_targets)
      {
        if (libso* ls = pt->is_a<libso> ())
        {
          if (ls->member == nullptr)
            continue;

          file& dll (static_cast<file&> (*ls->member));

          s.insert (&dll);
          collect_dlls (s, *ls);
        }
      }
    }

    target_state link::
    perform_update (action a, target& xt)
    {
      tracer trace ("cxx::link::perform_update");

      file& t (static_cast<file&> (xt));

      type lt (link_type (t));
      bool so (lt == type::so);

      // Update prerequisites.
      //
      bool update (execute_prerequisites (a, t, t.mtime ()));

      scope& rs (t.root_scope ());

      const string& cid (cast<string> (rs["cxx.id"]));
      const string& tsys (cast<string> (rs["cxx.target.system"]));
      const string& tclass (cast<string> (rs["cxx.target.class"]));

      const string& aid (lt == type::a
                         ? cast<string> (rs["bin.ar.id"])
                         : string ());

      // Check/update the dependency database.
      //
      depdb dd (t.path () + ".d");

      // First should come the rule name/version.
      //
      if (dd.expect ("cxx.link 1") != nullptr)
        l4 ([&]{trace << "rule mismatch forcing update of " << t;});

      lookup ranlib;

      // Then the linker checksum (ar/ranlib or C++ compiler).
      //
      if (lt == type::a)
      {
        ranlib = rs["config.bin.ranlib"];

        if (ranlib->empty ()) // @@ BC LT [null].
          ranlib = lookup ();

        const char* rl (
          ranlib
          ? cast<string> (rs["bin.ranlib.checksum"]).c_str ()
          : "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        if (dd.expect (cast<string> (rs["bin.ar.checksum"])) != nullptr)
          l4 ([&]{trace << "ar mismatch forcing update of " << t;});

        if (dd.expect (rl) != nullptr)
          l4 ([&]{trace << "ranlib mismatch forcing update of " << t;});
      }
      else
      {
        if (dd.expect (cast<string> (rs["cxx.checksum"])) != nullptr)
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
        if (aid == "msvc")
        {
          // Translate the compiler target CPU to the /MACHINE option value.
          //
          const string& tcpu (cast<string> (rs["cxx.target.cpu"]));

          const char* m (tcpu == "i386" || tcpu == "i686"  ? "/MACHINE:x86"   :
                         tcpu == "x86_64"                  ? "/MACHINE:x64"   :
                         tcpu == "arm"                     ? "/MACHINE:ARM"   :
                         tcpu == "arm64"                   ? "/MACHINE:ARM64" :
                         nullptr);

          if (m == nullptr)
            fail << "unable to translate CPU " << tcpu << " to /MACHINE";

          args.push_back (m);
        }
        else
        {
          // If the user asked for ranlib, don't try to do its function with -s.
          // Some ar implementations (e.g., the LLVM one) doesn't support
          // leading '-'.
          //
          args.push_back (ranlib ? "rc" : "rcs");
        }
      }
      else
      {
        append_options (args, t, "cxx.coptions");
        append_std (args, rs, cid, t, std);

        // Handle soname/rpath. Emulation for Windows is done after we have
        // built the target.
        //
        if (tclass == "windows")
        {
          auto l (t["bin.rpath"]);

          if (l && !l->empty ())
            fail << cast<string> (rs["cxx.target"]) << " does not have rpath";
        }
        else
        {
          // Set soname.
          //
          if (so)
          {
            const string& leaf (t.path ().leaf ().string ());

            if (tclass == "macosx")
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

          // Add rpaths. We used to first add the ones specified by the user
          // so that they take precedence. But that caused problems if we have
          // old versions of the libraries sitting in the rpath location
          // (e.g., installed libraries). And if you think about this, it's
          // probably correct to prefer libraries that we explicitly imported
          // to the ones found via rpath.
          //
          // Note also that if this is update for install, then we don't add
          // rpath of the imported libraries (i.e., we assume they are also
          // installed).
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
              // Since with this option the paths are not stored in the
              // library, we have to do this recursively (in fact, we don't
              // really need it for top-level libraries).
              //
              else if (tclass == "linux" || tclass == "freebsd")
                append_rpath_link (sargs, *ls);
            }
          }

          if (auto l = t["bin.rpath"])
            for (const dir_path& p: cast<dir_paths> (l))
              sargs.push_back ("-Wl,-rpath," + p.string ());
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
      // the checksum is faster and simpler. And we like simple.
      //
      {
        sha256 cs;

        for (target* pt: t.prerequisite_targets)
        {
          path_target* ppt;
          liba* a (nullptr);

          if ((ppt = pt->is_a<obja> ())  ||
              (ppt = pt->is_a<objso> ()) ||
              (lt != type::a &&
               ((ppt = a = pt->is_a<liba> ()) ||
                (ppt = pt->is_a<libso> ()))))
          {
            cs.append (ppt->path ().string ());

            // If this is a static library, link all the libraries it depends
            // on, recursively.
            //
            if (a != nullptr)
              hash_libraries (cs, *a);
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
      // options or input file set), or if the database is newer than the
      // target (interrupted update) then force the target update. Also
      // note this situation in the "from scratch" flag.
      //
      bool scratch (false);
      if (dd.writing () || dd.mtime () > t.mtime ())
        scratch = update = true;

      dd.close ();

      // If nothing changed, then we are done.
      //
      if (!update)
        return target_state::unchanged;

      // Ok, so we are updating. Finish building the command line.
      //
      string out; // Storage.

      // Translate paths to relative (to working directory) ones. This results
      // in easier to read diagnostics.
      //
      path relt (relative (t.path ()));

      switch (lt)
      {
      case type::e:
        {
          args[0] = cast<path> (rs["config.cxx"]).string ().c_str ();

          if (cid == "msvc")
          {
            uint64_t cver (cast<uint64_t> (rs["cxx.version.major"]));

            if (verb < 3)
              args.push_back ("/nologo");

            // The /Fe: option (executable file name) only became available in
            // VS2013/12.0.
            //
            if (cver >= 18)
            {
              args.push_back ("/Fe:");
              args.push_back (relt.string ().c_str ());
            }
            else
            {
              out = "/Fe" + relt.string ();
              args.push_back (out.c_str ());
            }
          }
          else
          {
            args.push_back ("-o");
            args.push_back (relt.string ().c_str ());
          }

          break;
        }
      case type::a:
        {
          //@@ VC: what are /LIBPATH, /NODEFAULTLIB for?
          //

          args[0] = cast<path> (rs["config.bin.ar"]).string ().c_str ();

          if (aid == "msvc")
          {
            if (verb < 3)
              args.push_back ("/NOLOGO");

            out = "/OUT:" + relt.string ();
            args.push_back (out.c_str ());
          }
          else
            args.push_back (relt.string ().c_str ());

          break;
        }
      case type::so:
        {
          args[0] = cast<path> (rs["config.cxx"]).string ().c_str ();

          if (cid == "msvc")
          {
            //@@ VC TODO: DLL building (names via /link?)
          }
          else
          {
            // Add the option that triggers building a shared library.
            //
            if (tclass == "macosx")
              args.push_back ("-dynamiclib");
            else
              args.push_back ("-shared");

            if (tsys == "mingw32")
            {
              // On Windows libso{} is the import stub and its first ad hoc
              // group member is dll{}.
              //
              out = "-Wl,--out-implib=" + relt.string ();
              relt = relative (static_cast<file*> (t.member)->path ());

              args.push_back ("-o");
              args.push_back (relt.string ().c_str ());
              args.push_back (out.c_str ());
            }
            else
            {
              args.push_back ("-o");
              args.push_back (relt.string ().c_str ());
            }
          }

          break;
        }
      }

      size_t oend (sargs.size ()); // Note the end of options in sargs.

      for (target* pt: t.prerequisite_targets)
      {
        path_target* ppt;
        liba* a (nullptr);
        libso* so (nullptr);

        if ((ppt = pt->is_a<obja> ())  ||
            (ppt = pt->is_a<objso> ()) ||
            (lt != type::a &&
             ((ppt = a = pt->is_a<liba> ()) ||
              (ppt = so = pt->is_a<libso> ()))))
        {
          sargs.push_back (relative (ppt->path ()).string ()); // string()&&

          // If this is a static library, link all the libraries it depends
          // on, recursively.
          //
          if (a != nullptr)
            append_libraries (sargs, *a);
        }
      }

      // Copy sargs to args. Why not do it as we go along pushing into sargs?
      // Because of potential realocations.
      //
      for (size_t i (0); i != sargs.size (); ++i)
      {
        if (lt != type::a && i == oend)
        {
          //@@ VC: TMP, until we use link.exe directly (would need to
          //   prefix them with /link otherwise).
          //
          if (cid != "msvc")
            append_options (args, t, "cxx.loptions");
        }

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
        //@@ VC: I think it prints each object file being added.
        //
        // Not for lib.exe
        //

        // VC++ (cl.exe, lib.exe, and link.exe) sends diagnostics to
        // stdout. To fix this (and any other insane compilers that may want
        // to do something like this) we are going to always redirect stdout
        // to stderr. For sane compilers this should be harmless.
        //
        process pr (args.data (), 0, 2);

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

      // Remove the target file if any of the subsequent actions fail. If we
      // don't do that, we will end up with a broken build that is up-to-date.
      //
      auto_rmfile rm (t.path ());

      if (ranlib)
      {
        const char* args[] = {
          cast<path> (ranlib).string ().c_str (),
          relt.string ().c_str (),
          nullptr};

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

      // Emulate rpath on Windows.
      //
      if (lt == type::e && tclass == "windows")
        emulate_rpath_windows (t, scratch, cid != "msvc");

      rm.cancel ();

      // Should we go to the filesystem and get the new mtime? We know the
      // file has been modified, so instead just use the current clock time.
      // It has the advantage of having the subseconds precision.
      //
      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    target_state link::
    perform_clean (action a, target& xt)
    {
      tracer trace ("cxx::link::perform_clean");

      file& t (static_cast<file&> (xt));

      type lt (link_type (t));

      scope& rs (t.root_scope ());
      const string& cid (cast<string> (rs["cxx.id"]));
      const string& tclass (cast<string> (rs["cxx.target.class"]));

      if (lt == type::e && tclass == "windows")
      {
        bool m (cid != "msvc");

        // Check for custom manifest, just like in emulate_rpath_windows().
        //
        if (m &&
            rs.src_path () == rs.out_path () &&
            file_exists (t.path () + ".manifest") &&
            !dir_exists (path_cast<dir_path> (t.path () + ".dlls")))
        {
          fail << t.path () + ".manifest" << " looks like a custom manifest" <<
            info << "remove it manually if that's not the case";
        }

        return clean_extra (
          a,
          t,
          {"+.d", (m ? "+.manifest" : nullptr), "/+.dlls"});
      }
      else
        return clean_extra (a, t, {"+.d"});
    }

    link link::instance;
  }
}
