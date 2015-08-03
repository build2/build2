// file      : build/cxx/link.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/link>

#include <vector>
#include <string>
#include <cstddef>  // size_t
#include <cstdlib>  // exit()
#include <utility>  // move()

#include <butl/process>
#include <butl/utility>  // reverse_iterate
#include <butl/fdstream>
#include <butl/optional>
#include <butl/path-map>
#include <butl/filesystem>

#include <build/types>
#include <build/scope>
#include <build/variable>
#include <build/algorithm>
#include <build/diagnostics>
#include <build/context>

#include <build/bin/target>
#include <build/cxx/target>

#include <build/cxx/utility>

using namespace std;
using namespace butl;

namespace build
{
  namespace cxx
  {
    using namespace bin;

    enum class type {e, a, so};
    enum class order {a, so, a_so, so_a};

    static inline type
    link_type (target& t)
    {
      return t.is_a<exe> () ? type::e : (t.is_a<liba> () ? type::a : type::so);
    }

    static order
    link_order (target& t)
    {
      const char* var;

      switch (link_type (t))
      {
      case type::e:  var = "bin.exe.lib";   break;
      case type::a:  var = "bin.liba.lib";  break;
      case type::so: var = "bin.libso.lib"; break;
      }

      const list_value& lv (t[var].as<const list_value&> ());
      return lv[0].value == "shared"
        ? lv.size () > 1 && lv[1].value == "static" ? order::so_a : order::so
        : lv.size () > 1 && lv[1].value == "shared" ? order::a_so : order::a;
    }

    link::search_paths link::
    extract_library_paths (scope& bs)
    {
      search_paths r;
      scope& rs (*bs.root_scope ());

      // Extract user-supplied search paths (i.e., -L).
      //
      if (auto val = bs["cxx.loptions"])
      {
        const list_value& l (val.as<const list_value&> ());

        for (auto i (l.begin ()), e (l.end ()); i != e; ++i)
        {
          if (!i->simple ())
            continue;

          // -L can either be in the -Lfoo or -L foo form.
          //
          dir_path d;
          if (i->value == "-L")
          {
            if (++i == e)
              break; // Let the compiler complain.

            if (i->simple ())
              d = dir_path (i->value);
            else if (i->directory ())
              d = i->dir;
            else
              break; // Let the compiler complain.
          }
          else if (i->value.compare (0, 2, "-L") == 0)
            d = dir_path (i->value, 2, string::npos);
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

      args.push_back (rs["config.cxx"].as<const string&> ().c_str ());
      append_options (args, bs, "cxx.coptions");
      append_std (args, bs, std_storage);
      append_options (args, bs, "cxx.loptions");
      args.push_back ("-print-search-dirs");
      args.push_back (nullptr);

      if (verb >= 5)
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
        an = path ("lib" + p.name);

        // Note that p.scope should be the same as the target's for
        // which we are looking for this library. The idea here is
        // that we have to use the same "extension configuration" as
        // the target's.
        //
        ae = ext == nullptr
          ? &liba::static_type.extension (p.key ().tk, p.scope)
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
        se = ext == nullptr
          ? &libso::static_type.extension (p.key ().tk, p.scope)
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
        else if (p.is_a<h> ()   ||
                 p.is_a<hxx> () ||
                 p.is_a<ixx> () ||
                 p.is_a<txx> () ||
                 p.is_a<fsdir> ())
          ;
        else
        {
          level3 ([&]{trace << "unexpected prerequisite type " << p.type ();});
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

      // If we have any prerequisite libraries (which also means that
      // we match), search/import and pre-match them to implement the
      // "library meta-information protocol".
      //
      if (seen_lib && lt != type::e)
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

      type lt (link_type (t));
      bool so (lt == type::so);
      optional<order> lo; // Link-order.

      // Derive file name from target name.
      //
      if (t.path ().empty ())
      {
        switch (lt)
        {
        case type::e:  t.derive_path (""         ); break;
        case type::a:  t.derive_path ("a",  "lib"); break;
        case type::so: t.derive_path ("so", "lib"); break;
        }
      }

      t.prerequisite_targets.clear (); // See lib pre-match in match() above.

      // Inject dependency on the output directory.
      //
      inject_parent_fsdir (a, t);

      // We may need the project roots for rule chaining (see below).
      // We will resolve them lazily only if needed.
      //
      scope* root (nullptr);
      const dir_path* out_root (nullptr);
      const dir_path* src_root (nullptr);

      search_paths_cache lib_paths; // Extract lazily.

      // Process prerequisites: do rule chaining for C and C++ source
      // files as well as search and match.
      //
      // When cleaning, ignore prerequisites that are not in the same
      // or a subdirectory of our strong amalgamation.
      //
      const dir_path* amlg (
        a.operation () != clean_id
        ? nullptr
        : &t.strong_scope ().path ());

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

          if (a.operation () == clean_id && !pt->dir.sub (*amlg))
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
            // Determine the library type to link.
            //
            bool lso (true);
            const string& at ((*l)["bin.lib"].as<const string&> ());

            if (!lo)
              lo = link_order (t);

            switch (*lo)
            {
            case order::a:
            case order::a_so:
              lso = false; // Fall through.
            case order::so:
            case order::so_a:
              {
                if (lso ? at == "static" : at == "shared")
                {
                  if (*lo == order::a_so || *lo == order::so_a)
                    lso = !lso;
                  else
                    fail << (lso ? "shared" : "static") << " build of " << *l
                         << " is not available";
                }
              }
            }

            pt = lso ? static_cast<target*> (l->so) : l->a;

            if (pt == nullptr)
              pt = &search (lso ? libso::static_type : liba::static_type,
                            p.key ());
          }

          build::match (a, *pt);
          t.prerequisite_targets.push_back (pt);
          continue;
        }

        if (root == nullptr)
        {
          // Which scope shall we use to resolve the root? Unlikely,
          // but possible, the prerequisite is from a different project
          // altogether. So we are going to use the target's project.
          //
          root = &t.root_scope ();
          out_root = &root->path ();
          src_root = &root->src_path ();
        }

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

          if (cpd.relative () || cpd.sub (*out_root))
            d = cpd;
          else
          {
            if (!cpd.sub (*src_root))
              fail << "out of project prerequisite " << cp <<
                info << "specify corresponding " << o_type.name << "{} "
                   << "target explicitly";

            d = *out_root / cpd.leaf (*src_root);
          }
        }

        target& ot (search (o_type, d, *cp.tk.name, nullptr, cp.scope));

        // If we are cleaning, check that this target is in the same or
        // a subdirectory of our strong amalgamation.
        //
        if (a.operation () == clean_id && !ot.dir.sub (*amlg))
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
            build::match (a, *pt); // Now p1 should be resolved.

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

          build::match (a, *pt);
        }

        t.prerequisite_targets.push_back (pt);
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

      type lt (link_type (t));
      bool so (lt == type::so);

      if (!execute_prerequisites (a, t, t.mtime ()))
        return target_state::unchanged;

      // Translate paths to relative (to working directory) ones. This
      // results in easier to read diagnostics.
      //
      path relt (relative (t.path ()));

      scope& rs (t.root_scope ());
      cstrings args;
      string storage1;

      if (lt == type::a)
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

      // Reserve enough space so that we don't reallocate. Reallocating
      // means pointers to elements may no longer be valid.
      //
      paths relo;
      relo.reserve (t.prerequisite_targets.size ());

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
        {
          // Use absolute path for the shared libraries since that's
          // the path the runtime loader will use to try to find it.
          // This is probably temporary until we get into the whole
          // -soname/-rpath mess.
          //
          args.push_back (ppt->path ().string ().c_str ());
          continue;
        }
        else
          continue;

        relo.push_back (relative (ppt->path ()));
        args.push_back (relo.back ().string ().c_str ());
      }

      if (lt != type::a)
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

    link link::instance;
  }
}
