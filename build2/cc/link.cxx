// file      : build2/cc/link.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/link>

#include <map>
#include <cstdlib>  // exit()
#include <iostream> // cerr

#include <butl/path-map>
#include <butl/filesystem> // file_exists()

#include <build2/depdb>
#include <build2/scope>
#include <build2/context>
#include <build2/variable>
#include <build2/algorithm>
#include <build2/filesystem>
#include <build2/diagnostics>

#include <build2/bin/target>

#include <build2/cc/target>  // c
#include <build2/cc/utility>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    link::
    link (data&& d)
        : common (move (d)),
          rule_id (string (x) += ".link 1")
    {
    }

    match_result link::
    match (action a, target& t, const string& hint) const
    {
      tracer trace (x, "link::match");

      // @@ TODO:
      //
      // - if path already assigned, verify extension?
      //
      // @@ Q:
      //
      // - if there is no .o, are we going to check if the one derived
      //   from target exist or can be built? A: No.
      //   What if there is a library. Probably ok if static, not if shared,
      //   (i.e., a utility library).
      //

      otype lt (link_type (t));

      // Scan prerequisites and see if we can work with what we've got. Note
      // that X could be C. We handle this by always checking for X first.
      //
      bool seen_x (false), seen_c (false), seen_obj (false), seen_lib (false);

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (p.is_a (x_src))
        {
          seen_x = seen_x || true;
        }
        else if (p.is_a<c> ())
        {
          seen_c = seen_c || true;
        }
        else if (p.is_a<obj> ())
        {
          seen_obj = seen_obj || true;
        }
        else if (p.is_a<obje> ())
        {
          if (lt != otype::e)
            fail << "obje{} as prerequisite of " << t;

          seen_obj = seen_obj || true;
        }
        else if (p.is_a<obja> ())
        {
          if (lt != otype::a)
            fail << "obja{} as prerequisite of " << t;

          seen_obj = seen_obj || true;
        }
        else if (p.is_a<objs> ())
        {
          if (lt != otype::s)
            fail << "objs{} as prerequisite of " << t;

          seen_obj = seen_obj || true;
        }
        else if (p.is_a<lib> ()  ||
                 p.is_a<liba> () ||
                 p.is_a<libs> ())
        {
          seen_lib = seen_lib || true;
        }
        // If this is some other c-common source (say C++ in a C rule), then
        // it will most definitely need to be compiled but we can't do that.
        //
        else if (p.is_a<cc> ())
          return false;
      }

      if (!(seen_x || seen_c || seen_obj || seen_lib))
        return false;

      // We will only chain a C source if there is also an X source or we were
      // explicitly told to.
      //
      if (seen_c && !seen_x && hint < x)
      {
        l4 ([&]{trace << "C prerequisite without " << x_lang << " or hint";});
        return false;
      }

      // Set the library type.
      //
      t.vars.assign (c_type) = string (x);

      // If we have any prerequisite libraries, search/import and pre-match
      // them to implement the "library meta-information protocol". Don't do
      // this if we are called from the install rule just to check if we would
      // match.
      //
      auto op (a.operation ());
      auto oop (a.outer_operation ());

      if (seen_lib && lt != otype::e &&
          op != install_id   && oop != install_id &&
          op != uninstall_id && oop != uninstall_id)
      {
        if (t.group != nullptr)
          t.group->prerequisite_targets.clear (); // lib{}'s

        optional<dir_paths> usr_lib_dirs; // Extract lazily.

        for (prerequisite_member p: group_prerequisite_members (a, t))
        {
          if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libs> ())
          {
            target* pt (nullptr);

            // Handle imported libraries.
            //
            if (p.proj () != nullptr)
              pt = search_library (sys_lib_dirs, usr_lib_dirs, p.prerequisite);

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

      return true;
    }

    auto link::
    derive_libs_paths (file& ls) const -> libs_paths
    {
      const char* ext (nullptr);
      const char* pfx (nullptr);
      const char* sfx (nullptr);

      bool win (tclass == "windows");

      if (win)
      {
        if (tsys == "mingw32")
          pfx = "lib";

        ext = "dll";
      }
      else if (tclass == "macosx")
      {
        pfx = "lib";
        ext = "dylib";
      }
      else
      {
        pfx = "lib";
        ext = "so";
      }

      if (auto l = ls["bin.lib.prefix"]) pfx = cast<string> (l).c_str ();
      if (auto l = ls["bin.lib.suffix"]) sfx = cast<string> (l).c_str ();

      // First sort out which extension we are using.
      //
      const string& e (ls.derive_extension (ext));

      auto append_ext = [&e] (path& p)
      {
        if (!e.empty ())
        {
          p += '.';
          p += e;
        }
      };

      // Figure out the version.
      //
      string v;
      using verion_map = map<string, string>;
      if (const verion_map* m = cast_null<verion_map> (ls["bin.lib.version"]))
      {
        // First look for the target system.
        //
        auto i (m->find (tsys));

        // Then look for the target class.
        //
        if (i == m->end ())
          i = m->find (tclass);

        // Then look for the wildcard. Since it is higly unlikely one can have
        // a version that will work across platforms, this is only useful to
        // say "all others -- no version".
        //
        if (i == m->end ())
          i = m->find ("*");

        // At this stage the only platform-specific version we support is the
        // "no version" override.
        //
        if (i != m->end () && !i->second.empty ())
          fail << i->first << "-specific bin.lib.version not yet supported";

        // Finally look for the platform-independent version.
        //
        if (i == m->end ())
          i = m->find ("");

        // If we didn't find anything, fail. If the bin.lib.version was
        // specified, then it should explicitly handle all the targets.
        //
        if (i == m->end ())
          fail << "no version for " << ctg << " in bin.lib.version" <<
            info << "considere adding " << tsys << "@<ver> or " << tclass
               << "@<ver>";

        v = i->second;
      }

      // Now determine the paths.
      //
      path lk, so, in;
      const path* re (nullptr);

      // We start with the basic path.
      //
      path b (ls.dir);
      {
        if (pfx == nullptr)
          b /= ls.name;
        else
        {
          b /= pfx;
          b += ls.name;
        }

        if (sfx != nullptr)
          b += sfx;
      }

      // On Windows the real path is to libs{} and the link path is to the
      // import library.
      //
      if (win)
      {
        // Usually on Windows the import library is called the same as the DLL
        // but with the .lib extension. Which means it clashes with the static
        // library. Instead of decorating the static library name with ugly
        // suffixes (as is customary), let's use the MinGW approach (one must
        // admit it's quite elegant) and call it .dll.lib.
        //
        lk = b;
        append_ext (lk);

        libi& li (static_cast<libi&> (*ls.member));
        lk = li.derive_path (move (lk), tsys == "mingw32" ? "a" : "lib");
      }
      else if (!v.empty ())
      {
        lk = b;
        append_ext (lk);
      }

      if (!v.empty ())
        b += v;

      re = &ls.derive_path (move (b));

      return libs_paths {move (lk), move (so), move (in), re};
    }

    recipe link::
    apply (action a, target& xt) const
    {
      tracer trace (x, "link::apply");

      file& t (static_cast<file&> (xt));

      scope& bs (t.base_scope ());
      scope& rs (*bs.root_scope ());

      otype lt (link_type (t));
      lorder lo (link_order (bs, lt));

      // Derive file name(s) and add ad hoc group members.
      //
      auto add_adhoc = [a, &bs] (target& t, const char* type) -> file&
      {
        const target_type& tt (*bs.find_target_type (type));

        if (t.member != nullptr) // Might already be there.
          assert (t.member->type () == tt);
        else
          t.member = &search (tt, t.dir, t.out, t.name, nullptr, nullptr);

        file& r (static_cast<file&> (*t.member));
        r.recipe (a, group_recipe);
        return r;
      };

      {
        const char* e (nullptr); // Extension.
        const char* p (nullptr); // Prefix.
        const char* s (nullptr); // Suffix.

        switch (lt)
        {
        case otype::e:
          {
            if (tclass == "windows")
              e = "exe";
            else
              e = "";

            if (auto l = t["bin.exe.prefix"]) p = cast<string> (l).c_str ();
            if (auto l = t["bin.exe.suffix"]) s = cast<string> (l).c_str ();

            t.derive_path (e, p, s);
            break;
          }
        case otype::a:
          {
            if (cid == "msvc")
              e = "lib";
            else
            {
              p = "lib";
              e = "a";
            }

            if (auto l = t["bin.lib.prefix"]) p = cast<string> (l).c_str ();
            if (auto l = t["bin.lib.suffix"]) s = cast<string> (l).c_str ();

            t.derive_path (e, p, s);
            break;
          }
        case otype::s:
          {
            // On Windows libs{} is an ad hoc group. The libs{} itself is the
            // DLL and we add libi{} import library as its member.
            //
            if (tclass == "windows")
              add_adhoc (t, "libi");

            derive_libs_paths (t);
            break;
          }
        }
      }

      // PDB
      //
      if (lt != otype::a &&
          cid == "msvc" &&
          (find_option ("/DEBUG", t, c_loptions, true) ||
           find_option ("/DEBUG", t, x_loptions, true)))
      {
        // Add after the import library if any.
        //
        file& pdb (add_adhoc (t.member == nullptr ? t : *t.member, "pdb"));

        // We call it foo.{exe,dll}.pdb rather than just foo.pdb because we
        // can have both foo.exe and foo.dll in the same directory.
        //
        pdb.derive_path (t.path (), "pdb");
      }

      t.prerequisite_targets.clear (); // See lib pre-match in match() above.

      // Inject dependency on the output directory.
      //
      inject_fsdir (a, t);

      optional<dir_paths> usr_lib_dirs; // Extract lazily.

      // Process prerequisites: do rule chaining for C and X source files as
      // well as search and match.
      //
      // When cleaning, ignore prerequisites that are not in the same or a
      // subdirectory of our project root.
      //
      const target_type& ott (lt == otype::e ? obje::static_type :
                              lt == otype::a ? obja::static_type :
                              objs::static_type);

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        target* pt (nullptr);

        if (!p.is_a (x_src) && !p.is_a<c> ())
        {
          // Handle imported libraries.
          //
          if (p.proj () != nullptr)
            pt = search_library (sys_lib_dirs, usr_lib_dirs, p.prerequisite);

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
            switch (lt)
            {
            case otype::e: pt = o->e; break;
            case otype::a: pt = o->a; break;
            case otype::s: pt = o->s; break;
            }

            if (pt == nullptr)
              pt = &search (ott, p.key ());
          }
          else if (lib* l = pt->is_a<lib> ())
          {
            pt = &link_member (*l, lo);
          }

          build2::match (a, *pt);
          t.prerequisite_targets.push_back (pt);
          continue;
        }

        // The rest is rule chaining.
        //

        // Which scope shall we use to resolve the root? Unlikely, but
        // possible, the prerequisite is from a different project
        // altogether. So we are going to use the target's project.
        //

        // @@ Why are we creating the obj{} group if the source came from a
        //    group?
        //
        bool group (!p.prerequisite.belongs (t)); // Group's prerequisite.

        const prerequisite_key& cp (p.key ()); // C-source (X or C) key.
        const target_type& tt (group ? obj::static_type : ott);

        // Come up with the obj*{} target. The source prerequisite directory
        // can be relative (to the scope) or absolute. If it is relative, then
        // use it as is. If absolute, then translate it to the corresponding
        // directory under out_root. While the source directory is most likely
        // under src_root, it is also possible it is under out_root (e.g.,
        // generated source).
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
                info << "specify corresponding " << tt.name << "{} "
                   << "target explicitly";

            d = rs.out_path () / cpd.leaf (rs.src_path ());
          }
        }

        // obj*{} is always in the out tree.
        //
        target& ot (
          search (tt, d, dir_path (), *cp.tk.name, nullptr, cp.scope));

        // If we are cleaning, check that this target is in the same or
        // a subdirectory of our project root.
        //
        if (a.operation () == clean_id && !ot.dir.sub (rs.out_path ()))
        {
          // If we shouldn't clean obj{}, then it is fair to assume we
          // shouldn't clean the source either (generated source will be in
          // the same directory as obj{} and if not, well, go find yourself
          // another build system ;-)).
          //
          continue; // Skip.
        }

        // If we have created the obj{} target group, pick one of its members;
        // the rest would be primarily concerned with it.
        //
        if (group)
        {
          obj& o (static_cast<obj&> (ot));

          switch (lt)
          {
          case otype::e: pt = o.e; break;
          case otype::a: pt = o.a; break;
          case otype::s: pt = o.s; break;
          }

          if (pt == nullptr)
            pt = &search (ott, o.dir, o.out, o.name, o.ext, nullptr);
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
          // Most of the time we will have just a single source so fast-path
          // that case.
          //
          if (p1.is_a (x_src) || p1.is_a<c> ())
          {
            if (!found)
            {
              build2::match (a, *pt); // Now p1 should be resolved.

              // Searching our own prerequisite is ok.
              //
              if (&p.search () != &p1.search ())
                fail << "synthesized target for prerequisite " << cp << " "
                     << "would be incompatible with existing target " << *pt <<
                  info << "existing prerequisite " << p1 << " does not match "
                     << cp <<
                  info << "specify corresponding " << tt.name << "{} target "
                     << "explicitly";

              found = true;
            }

            continue; // Check the rest of the prerequisites.
          }

          // Ignore some known target types (fsdir, headers, libraries).
          //
          if (p1.is_a<fsdir> () ||
              p1.is_a<lib>  ()  ||
              p1.is_a<liba> ()  ||
              p1.is_a<libs> ()  ||
              (p.is_a (x_src) && x_header (p1)) ||
              (p.is_a<c> () && p1.is_a<h> ()))
            continue;

          fail << "synthesized target for prerequisite " << cp
               << " would be incompatible with existing target " << *pt <<
            info << "unexpected existing prerequisite type " << p1 <<
            info << "specify corresponding obj{} target explicitly";
        }

        if (!found)
        {
          // Note: add the source to the group, not the member.
          //
          ot.prerequisites.emplace_back (p.as_prerequisite (trace));

          // Add our lib*{} prerequisites to the object file (see the export.*
          // machinery for details).
          //
          // Note that we don't resolve lib{} to liba{}/libs{} here instead
          // leaving it to whoever (e.g., the compile rule) will be needing
          // *.export.*. One reason for doing it there is that the object
          // target might be specified explicitly by the user in which case
          // they will have to specify the set of lib{} prerequisites and it's
          // much cleaner to do as lib{} rather than liba{}/libs{}.
          //
          // Initially, we were only adding imported libraries, but there is a
          // problem with this approach: the non-imported library might depend
          // on the imported one(s) which we will never "see" unless we start
          // with this library.
          //
          for (prerequisite& p: group_prerequisites (t))
          {
            if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libs> ())
              ot.prerequisites.emplace_back (p);
          }

          build2::match (a, *pt);
        }

        t.prerequisite_targets.push_back (pt);
      }

      switch (a)
      {
      case perform_update_id:
        return [this] (action a, target& t) {return perform_update (a, t);};
      case perform_clean_id:
        return [this] (action a, target& t) {return perform_clean (a, t);};
      default:
        return noop_recipe; // Configure update.
      }
    }

    void link::
    append_libraries (strings& args,
                      file& l, bool la,
                      scope& bs, lorder lo) const
    {
      // Note: lack of the "small function object" optimization will really
      // kill us here since we are called in a loop.
      //
      bool win (tclass == "windows");

      auto imp = [] (file&, bool la) {return la;};

      auto lib = [&args, win] (file* f, const string& p, bool)
      {
        if (f != nullptr)
        {
          // On Windows a shared library is a DLL with the import library as a
          // first ad hoc group member. MinGW though can link directly to DLLs
          // (see search_library() for details).
          //
          if (win && f->member != nullptr && f->is_a<libs> ())
            f = static_cast<file*> (f->member);

          args.push_back (relative (f->path ()).string ());
        }
        else
          args.push_back (p);
      };

      auto opt = [&args, this] (file& l, const string& t, bool com, bool exp)
      {
        // If we need an interface value, then use the group (lib{}).
        //
        if (target* g = exp && l.is_a<libs> () ? l.group : &l)
        {
          const variable& var (
            com
            ? (exp ? c_export_loptions : c_loptions)
            : (t == x
               ? (exp ? x_export_loptions : x_loptions)
               : var_pool[t + (exp ? ".export.loptions" : ".loptions")]));

          append_options (args, *g, var);
        }
      };

      process_libraries (bs, lo, sys_lib_dirs, l, la, imp, lib, opt, true);
    }

    void link::
    hash_libraries (sha256& cs, file& l, bool la, scope& bs, lorder lo) const
    {
      bool win (tclass == "windows");

      auto imp = [] (file&, bool la) {return la;};

      auto lib = [&cs, win] (file* f, const string& p, bool)
      {
        if (f != nullptr)
        {
          // On Windows a shared library is a DLL with the import library as a
          // first ad hoc group member. MinGW though can link directly to DLLs
          // (see search_library() for details).
          //
          if (win && f->member != nullptr && f->is_a<libs> ())
            f = static_cast<file*> (f->member);

          cs.append (f->path ().string ());
        }
        else
          cs.append (p);
      };

      auto opt = [&cs, this] (file& l, const string& t, bool com, bool exp)
      {
        if (target* g = exp && l.is_a<libs> () ? l.group : &l)
        {
          const variable& var (
            com
            ? (exp ? c_export_loptions : c_loptions)
            : (t == x
               ? (exp ? x_export_loptions : x_loptions)
               : var_pool[t + (exp ? ".export.loptions" : ".loptions")]));

          hash_options (cs, *g, var);
        }
      };

      process_libraries (bs, lo, sys_lib_dirs, l, la, imp, lib, opt, true);
    }

    void link::
    rpath_libraries (strings& args,
                     target& t, scope& bs, lorder lo,
                     bool for_install) const
    {
      // Use -rpath-link on targets that support it (Linux, *BSD). Note
      // that we don't really need it for top-level libraries.
      //
      if (for_install)
      {
        if (tclass != "linux" && tclass != "bsd")
          return;
      }

      auto imp = [for_install] (file&, bool la)
      {
        // If we are not installing, then we only need to rpath interface
        // libraries (they will include rpath's for their implementations).
        // Otherwise, we have to do this recursively.
        //
        // The rpath-link part is tricky: ideally we would like to get only
        // implementations and only of shared libraries. We are not interested
        // in interfaces because we are linking their libraries explicitly.
        // However, in our model there is no such thing as "implementation
        // only"; it is either interface or interface and implementation. So
        // we are going to rpath-link all of them which should be harmless
        // except for some noise on the command line.
        //
        return for_install ? !la : false;
      };

      // Package the data to keep within the 2-pointer small std::function
      // optimization limit.
      //
      struct
      {
        strings& args;
        bool for_install;
      } d {args, for_install};

      auto lib = [&d, this] (file* l, const string& f, bool sys)
      {
        // We don't rpath system libraries. Why, you may ask? There are many
        // good reasons and I have them written on an napkin somewhere...
        //
        if (sys)
          return;

        if (l != nullptr)
        {
          if (!l->is_a<libs> ())
            return;
        }
        else
        {
          // This is an absolute path and we need to decide whether it is
          // a shared or static library. Doesn't seem there is anything
          // better than checking for a platform-specific extension (maybe
          // we should cache it somewhere).
          //
          size_t p (path::traits::find_extension (f));

          if (p == string::npos)
            return;

          ++p; // Skip dot.

          bool c (true);
          const char* e;

          if      (tclass == "windows") {e = "dll"; c = false;}
          else if (tsys == "darwin")     e = "dylib";
          else                           e = "so";

          if ((c
               ? f.compare (p, string::npos, e)
               : casecmp (f.c_str () + p, e)) != 0)
            return;
        }

        // Ok, if we are here then it means we have a non-system, shared
        // library and its absolute path is in f.
        //
        string o (d.for_install ? "-Wl,-rpath-link," : "-Wl,-rpath,");

        size_t p (path::traits::rfind_separator (f));
        assert (p != string::npos);

        o.append (f, 0, (p != 0 ? p : 1)); // Don't include trailing slash.
        d.args.push_back (move (o));
      };

      // In case we don't have the "small function object" optimization.
      //
      const function<bool (file&, bool)> impf (imp);
      const function<void (file*, const string&, bool)> libf (lib);

      for (target* pt: t.prerequisite_targets)
      {
        file* f;
        liba* a;

        if ((f = a = pt->is_a<liba> ()) ||
            (f =     pt->is_a<libs> ()))
        {
          if (!for_install && a == nullptr)
          {
            // Top-level sharen library dependency. It is either matched or
            // imported so should be a cc library.
            //
            if (!cast_false<bool> (f->vars[c_system]))
              args.push_back (
                "-Wl,-rpath," + f->path ().directory ().string ());
          }

          process_libraries (bs, lo, sys_lib_dirs,
                             *f, a != nullptr,
                             impf, libf, nullptr);
        }
      }
    }

    // Filter link.exe noise (msvc.cxx).
    //
    void
    msvc_filter_link (ifdstream&, const file&, otype);

    // Translate target CPU to /MACHINE option.
    //
    const char*
    msvc_machine (const string& cpu); // msvc.cxx

    target_state link::
    perform_update (action a, target& xt) const
    {
      tracer trace (x, "link::perform_update");

      auto oop (a.outer_operation ());
      bool for_install (oop == install_id || oop == uninstall_id);

      file& t (static_cast<file&> (xt));

      scope& bs (t.base_scope ());
      scope& rs (*bs.root_scope ());

      otype lt (link_type (t));
      lorder lo (link_order (bs, lt));

      // Update prerequisites.
      //
      bool update (execute_prerequisites (a, t, t.mtime ()));

      // If targeting Windows, take care of the manifest.
      //
      path manifest; // Manifest itself (msvc) or compiled object file.
      timestamp rpath_timestamp (timestamp_nonexistent); // DLLs timestamp.

      if (lt == otype::e && tclass == "windows")
      {
        // First determine if we need to add our rpath emulating assembly. The
        // assembly itself is generated later, after updating the target. Omit
        // it if we are updating for install.
        //
        if (!for_install)
          rpath_timestamp = windows_rpath_timestamp (t, bs, lo);

        path mf (
          windows_manifest (
            t,
            rpath_timestamp != timestamp_nonexistent));

        timestamp mt (file_mtime (mf));

        if (tsys == "mingw32")
        {
          // Compile the manifest into the object file with windres. While we
          // are going to synthesize an .rc file to pipe to windres' stdin, we
          // will still use .manifest to check if everything is up-to-date.
          //
          manifest = mf + ".o";

          if (mt > file_mtime (manifest))
          {
            path of (relative (manifest));

            const process_path& rc (cast<process_path> (rs["bin.rc.path"]));

            // @@ Would be good to add this to depdb (e.g,, rc changes).
            //
            const char* args[] = {
              rc.recall_string (),
              "--input-format=rc",
              "--output-format=coff",
              "-o", of.string ().c_str (),
              nullptr};

            if (verb >= 3)
              print_process (args);

            try
            {
              process pr (rc, args, -1);

              try
              {
                ofdstream os (pr.out_fd);

                // 1 is resource ID, 24 is RT_MANIFEST. We also need to escape
                // Windows path backslashes.
                //
                os << "1 24 \"";

                const string& s (mf.string ());
                for (size_t i (0), j;; i = j + 1)
                {
                  j = s.find ('\\', i);
                  os.write (s.c_str () + i,
                            (j == string::npos ? s.size () : j) - i);

                  if (j == string::npos)
                    break;

                  os.write ("\\\\", 2);
                }

                os << "\"" << endl;

                os.close ();

                if (!pr.wait ())
                  throw failed (); // Assume diagnostics issued.
              }
              catch (const io_error& e)
              {
                if (pr.wait ()) // Ignore if child failed.
                  fail << "unable to pipe resource file to " << args[0]
                       << ": " << e.what ();
              }
            }
            catch (const process_error& e)
            {
              error << "unable to execute " << args[0] << ": " << e.what ();

              if (e.child ())
                exit (1);

              throw failed ();
            }

            update = true; // Manifest changed, force update.
          }
        }
        else
        {
          manifest = move (mf); // Save for link.exe's /MANIFESTINPUT.

          if (mt > t.mtime ())
            update = true; // Manifest changed, force update.
        }
      }

      // Check/update the dependency database.
      //
      depdb dd (t.path () + ".d");

      // First should come the rule name/version.
      //
      if (dd.expect (rule_id) != nullptr)
        l4 ([&]{trace << "rule mismatch forcing update of " << t;});

      lookup ranlib;

      // Then the linker checksum (ar/ranlib or the compiler).
      //
      if (lt == otype::a)
      {
        ranlib = rs["bin.ranlib.path"];

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
        // For VC we use link.exe directly.
        //
        const string& cs (
          cast<string> (
            rs[cid == "msvc" ? var_pool["bin.ld.checksum"] : x_checksum]));

        if (dd.expect (cs) != nullptr)
          l4 ([&]{trace << "linker mismatch forcing update of " << t;});
      }

      // Next check the target. While it might be incorporated into the linker
      // checksum, it also might not (e.g., VC link.exe).
      //
      if (dd.expect (ctg) != nullptr)
        l4 ([&]{trace << "target mismatch forcing update of " << t;});

      // Start building the command line. While we don't yet know whether we
      // will really need it, we need to hash it to find out. So the options
      // are to either replicate the exact process twice, first for hashing
      // then for building or to go ahead and start building and hash the
      // result. The first approach is probably more efficient while the
      // second is simpler. Let's got with the simpler for now (actually it's
      // kind of a hybrid).
      //
      cstrings args {nullptr}; // Reserve one for config.bin.ar/config.x.

      libs_paths paths;
      if (lt == otype::s)
        paths = derive_libs_paths (t);

      // Storage.
      //
      string soname1, soname2;
      strings sargs;

      if (lt == otype::a)
      {
        if (cid == "msvc") ;
        else
        {
          // If the user asked for ranlib, don't try to do its function with
          // -s.  Some ar implementations (e.g., the LLVM one) don't support
          // leading '-'.
          //
          args.push_back (ranlib ? "rc" : "rcs");
        }
      }
      else
      {
        if (cid == "msvc")
        {
          // We are using link.exe directly so don't pass the compiler
          // options.
        }
        else
        {
          append_options (args, t, c_coptions);
          append_options (args, t, x_coptions);
          append_std (args);
        }

        append_options (args, t, c_loptions);
        append_options (args, t, x_loptions);

        // Handle soname/rpath.
        //
        if (tclass == "windows")
        {
          // Limited emulation for Windows with no support for user-defined
          // rpaths.
          //
          auto l (t["bin.rpath"]);

          if (l && !l->empty ())
            fail << ctg << " does not support rpath";
        }
        else
        {
          // Set soname.
          //
          if (lt == otype::s)
          {
            const string& leaf (paths.effect_soname ().leaf ().string ());

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
          // installed). But we add -rpath-link for some platforms.
          //
          rpath_libraries (sargs, t, bs, lo, for_install);

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
          file* f;
          liba* a (nullptr);
          libs* s (nullptr);

          if ((f = pt->is_a<obje> ()) ||
              (f = pt->is_a<obja> ()) ||
              (f = pt->is_a<objs> ()) ||
              (lt != otype::a &&
               ((f = a = pt->is_a<liba> ()) ||
                (f = s = pt->is_a<libs> ()))))
          {
            // Link all the dependent interface libraries (shared) or interface
            // and implementation (static), recursively.
            //
            if (a != nullptr || s != nullptr)
              hash_libraries (cs, *f, a != nullptr, bs, lo);
            else
              cs.append (f->path ().string ());
          }
        }

        // Treat it as input for both MinGW and VC.
        //
        if (!manifest.empty ())
          cs.append (manifest.string ());

        // Treat them as inputs, not options.
        //
        if (lt != otype::a)
        {
          hash_options (cs, t, c_libs);
          hash_options (cs, t, x_libs);
        }

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "file set mismatch forcing update of " << t;});
      }

      // If any of the above checks resulted in a mismatch (different linker,
      // options or input file set), or if the database is newer than the
      // target (interrupted update) then force the target update. Also note
      // this situation in the "from scratch" flag.
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
      string out, out1, out2, out3; // Storage.

      // Translate paths to relative (to working directory) ones. This results
      // in easier to read diagnostics.
      //
      path relt (relative (t.path ()));

      const process_path* ld (nullptr);
      switch (lt)
      {
      case otype::a:
        {
          ld = &cast<process_path> (rs["bin.ar.path"]);

          if (cid == "msvc")
          {
            // lib.exe has /LIBPATH but it's not clear/documented what it's
            // used for. Perhaps for link-time code generation (/LTCG)? If
            // that's the case, then we may need to pass *.loptions.
            //
            args.push_back ("/NOLOGO");

            // Add /MACHINE.
            //
            args.push_back (msvc_machine (cast<string> (rs[x_target_cpu])));

            out = "/OUT:" + relt.string ();
            args.push_back (out.c_str ());
          }
          else
            args.push_back (relt.string ().c_str ());

          break;
        }
        // The options are usually similar enough to handle them together.
        //
      case otype::e:
      case otype::s:
        {
          if (cid == "msvc")
          {
            // Using link.exe directly.
            //
            ld = &cast<process_path> (rs["bin.ld.path"]);
            args.push_back ("/NOLOGO");

            if (lt == otype::s)
              args.push_back ("/DLL");

            // Add /MACHINE.
            //
            args.push_back (msvc_machine (cast<string> (rs[x_target_cpu])));

            // Unless explicitly enabled with /INCREMENTAL, disable
            // incremental linking (it is implicitly enabled if /DEBUG is
            // specified). The reason is the .ilk file: its name cannot be
            // changed and if we have, say, foo.exe and foo.dll, then they
            // will end up stomping on each other's .ilk's.
            //
            // So the idea is to disable it by default but let the user
            // request it explicitly if they are sure their project doesn't
            // suffer from the above issue. We can also have something like
            // 'incremental' config initializer keyword for this.
            //
            // It might also be a good idea to ask Microsoft to add an option.
            //
            if (!find_option ("/INCREMENTAL", args, true))
              args.push_back ("/INCREMENTAL:NO");

            // If you look at the list of libraries Visual Studio links by
            // default, it includes everything and a couple of kitchen sinks
            // (winspool32.lib, ole32.lib, odbc32.lib, etc) while we want to
            // keep our low-level build as pure as possible. However, there
            // seem to be fairly essential libraries that are not linked by
            // link.exe by default (use /VERBOSE:LIB to see the list). For
            // example, MinGW by default links advapi32, shell32, user32, and
            // kernel32. And so we follow suit and make sure those are linked.
            // advapi32 and kernel32 are already on the default list and we
            // only need to add the other two.
            //
            // The way we are going to do it is via the /DEFAULTLIB option
            // rather than specifying the libraries as normal inputs (as VS
            // does). This way the user can override our actions with the
            // /NODEFAULTLIB option.
            //
            args.push_back ("/DEFAULTLIB:shell32.lib");
            args.push_back ("/DEFAULTLIB:user32.lib");

            // Take care of the manifest (will be empty for the DLL).
            //
            if (!manifest.empty ())
            {
              out3 = "/MANIFESTINPUT:";
              out3 += relative (manifest).string ();
              args.push_back ("/MANIFEST:EMBED");
              args.push_back (out3.c_str ());
            }

            if (lt == otype::s)
            {
              // On Windows libs{} is the DLL and its first ad hoc group
              // member is the import library.
              //
              // This will also create the .exp export file. Its name will be
              // derived from the import library by changing the extension.
              // Lucky for us -- there is no option to name it.
              //
              auto imp (static_cast<file*> (t.member));
              out2 = "/IMPLIB:" + relative (imp->path ()).string ();
              args.push_back (out2.c_str ());
            }

            // If we have /DEBUG then name the .pdb file. It is either the
            // first (exe) or the second (dll) ad hoc group member.
            //
            if (find_option ("/DEBUG", args, true))
            {
              auto pdb (static_cast<file*> (
                          lt == otype::e ? t.member : t.member->member));
              out1 = "/PDB:" + relative (pdb->path ()).string ();
              args.push_back (out1.c_str ());
            }

            // @@ An executable can have an import library and VS seems to
            //    always name it. I wonder what would trigger its generation?
            //    Could it be the presence of export symbols? Yes, link.exe
            //    will generate the import library iff there are exported
            //    symbols. Which means there could be a DLL without an import
            //    library (which we currently don't handle very well).
            //
            out = "/OUT:" + relt.string ();
            args.push_back (out.c_str ());
          }
          else
          {
            ld = &cast<process_path> (rs[x_path]);

            // Add the option that triggers building a shared library and take
            // care of any extras (e.g., import library).
            //
            if (lt == otype::s)
            {
              if (tclass == "macosx")
                args.push_back ("-dynamiclib");
              else
                args.push_back ("-shared");

              if (tsys == "mingw32")
              {
                // On Windows libs{} is the DLL and its first ad hoc group
                // member is the import library.
                //
                auto imp (static_cast<file*> (t.member));
                out = "-Wl,--out-implib=" + relative (imp->path ()).string ();
                args.push_back (out.c_str ());
              }
            }

            args.push_back ("-o");
            args.push_back (relt.string ().c_str ());
          }

          break;
        }
      }

      args[0] = ld->recall_string ();

      for (target* pt: t.prerequisite_targets)
      {
        file* f;
        liba* a (nullptr);
        libs* s (nullptr);

        if ((f = pt->is_a<obje> ()) ||
            (f = pt->is_a<obja> ()) ||
            (f = pt->is_a<objs> ()) ||
            (lt != otype::a &&
             ((f = a = pt->is_a<liba> ()) ||
              (f = s = pt->is_a<libs> ()))))
        {
          // Link all the dependent interface libraries (shared) or interface
          // and implementation (static), recursively.
          //
          if (a != nullptr || s != nullptr)
            append_libraries (sargs, *f, a != nullptr, bs, lo);
          else
            sargs.push_back (relative (f->path ()).string ()); // string()&&
        }
      }

      // For MinGW manifest is an object file.
      //
      if (!manifest.empty () && tsys == "mingw32")
        sargs.push_back (relative (manifest).string ());

      // Copy sargs to args. Why not do it as we go along pushing into sargs?
      // Because of potential reallocations.
      //
      for (size_t i (0); i != sargs.size (); ++i)
        args.push_back (sargs[i].c_str ());

      if (lt != otype::a)
      {
        append_options (args, t, c_libs);
        append_options (args, t, x_libs);
      }

      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "ld " << t;

      try
      {
        // VC tools (both lib.exe and link.exe) send diagnostics to stdout.
        // Also, link.exe likes to print various gratuitous messages. So for
        // link.exe we redirect stdout to a pipe, filter that noise out, and
        // send the rest to stderr.
        //
        // For lib.exe (and any other insane compiler that may try to pull off
        // something like this) we are going to redirect stdout to stderr. For
        // sane compilers this should be harmless.
        //
        bool filter (cid == "msvc" && lt != otype::a);

        process pr (*ld, args.data (), 0, (filter ? -1 : 2));

        if (filter)
        {
          try
          {
            ifdstream is (pr.in_ofd, fdstream_mode::text, ifdstream::badbit);

            msvc_filter_link (is, t, lt);

            // If anything remains in the stream, send it all to stderr. Note
            // that the eof check is important: if the stream is at eof, this
            // and all subsequent writes to cerr will fail (and you won't see
            // a thing).
            //
            if (is.peek () != ifdstream::traits_type::eof ())
              cerr << is.rdbuf ();

            is.close ();
          }
          catch (const io_error&) {} // Assume exits with error.
        }

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
      auto_rmfile rm (relt);

      if (ranlib)
      {
        const process_path& rl (cast<process_path> (ranlib));

        const char* args[] = {
          rl.recall_string (),
          relt.string ().c_str (),
          nullptr};

        if (verb >= 2)
          print_process (args);

        try
        {
          process pr (rl, args);

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

      if (tclass == "windows")
      {
        // For Windows generate rpath-emulating assembly (unless updaing for
        // install).
        //
        if (lt == otype::e && !for_install)
          windows_rpath_assembly (t, bs, lo,
                                  cast<string> (rs[x_target_cpu]),
                                  rpath_timestamp,
                                  scratch);
      }
      else if (lt == otype::s)
      {
        // For shared libraries we may need to create a bunch of symlinks.
        //
        auto ln = [] (const path& f, const path& l)
        {
          // Note that we don't bother making the paths relative since they
          // will only be seen at verbosity level 3.
          //
          if (verb >= 3)
            text << "ln -sf " << f << ' ' << l;

          try
          {
            if (file_exists (l, false)) // The -f part.
              try_rmfile (l);

            mksymlink (f, l);
          }
          catch (const system_error& e)
          {
            fail << "unable to create symlink " << l << ": " << e.what ();
          }
        };

        const path& lk (paths.link);
        const path& so (paths.soname);
        const path& in (paths.interm);

        const path* f (paths.real);

        if (!in.empty ()) {ln (f->leaf (), in); f = &in;}
        if (!so.empty ()) {ln (f->leaf (), so); f = &so;}
        if (!lk.empty ()) {ln (f->leaf (), lk);}
      }

      rm.cancel ();

      // Should we go to the filesystem and get the new mtime? We know the
      // file has been modified, so instead just use the current clock time.
      // It has the advantage of having the subseconds precision.
      //
      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    target_state link::
    perform_clean (action a, target& xt) const
    {
      file& t (static_cast<file&> (xt));

      libs_paths paths;

      switch (link_type (t))
      {
      case otype::a:
        break; // Default.
      case otype::e:
        {
          if (tclass == "windows")
          {
            if (tsys == "mingw32")
              return clean_extra (
                a, t, {".d", ".dlls/", ".manifest.o", ".manifest"});
            else
              // Assuming it's VC or alike. Clean up .ilk in case the user
              // enabled incremental linking (note that .ilk replaces .exe).
              //
              return clean_extra (
                a, t, {".d", ".dlls/", ".manifest", "-.ilk"});
          }

          break;
        }
      case otype::s:
        {
          if (tclass == "windows")
          {
            // Assuming it's VC or alike. Clean up .exp and .ilk.
            //
            // Note that .exp is based on the .lib, not .dll name. And with
            // versioning their bases may not be the same.
            //
            if (tsys != "mingw32")
              return clean_extra (a, t, {{".d", "-.ilk"}, {"-.exp"}});
          }
          else
          {
            // Here we can have a bunch of symlinks that we need to remove. If
            // the paths are empty, then they will be ignored.
            //
            paths = derive_libs_paths (t);
            return clean_extra (a, t, {".d",
                                       paths.link.string ().c_str (),
                                       paths.soname.string ().c_str (),
                                       paths.interm.string ().c_str ()});
          }

          break;
        }
      }

      return clean_extra (a, t, {".d"});
    }
  }
}
