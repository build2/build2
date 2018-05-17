// file      : build2/cc/link-rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/link-rule.hxx>

#include <map>
#include <cstdlib>  // exit()

#include <libbutl/path-map.mxx>
#include <libbutl/filesystem.mxx> // file_exists()

#include <build2/depdb.hxx>
#include <build2/scope.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/target.hxx>  // c, pc*
#include <build2/cc/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    link_rule::
    link_rule (data&& d)
        : common (move (d)),
          rule_id (string (x) += ".link 1")
    {
      static_assert (sizeof (match_data) <= target::data_size,
                     "insufficient space");
    }

    bool link_rule::
    match (action a, target& t, const string& hint) const
    {
      tracer trace (x, "link_rule::match");

      // NOTE: may be called multiple times and for both inner and outer
      //       operations (see install rules).

      ltype lt (link_type (t));
      otype ot (lt.type);

      // If this is a library, link-up to our group (this is the target group
      // protocol which means this can be done whether we match or not).
      //
      // If we are called for the outer operation (see install rules), then
      // use resolve_group() to delegate to inner.
      //
      if (lt.library ())
      {
        if (a.outer ())
          resolve_group (a, t);
        else if (t.group == nullptr)
          t.group = &search (t,
                             lt.utility ? libu::static_type : lib::static_type,
                             t.dir, t.out, t.name);
      }

      // Scan prerequisites and see if we can work with what we've got. Note
      // that X could be C. We handle this by always checking for X first.
      //
      // Note also that we treat bmi{} as obj{}.
      //
      bool seen_x (false), seen_c (false), seen_obj (false), seen_lib (false);

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (p.is_a (x_src) || (x_mod != nullptr && p.is_a (*x_mod)))
        {
          seen_x = seen_x || true;
        }
        else if (p.is_a<c> ())
        {
          seen_c = seen_c || true;
        }
        else if (p.is_a<obj> () || p.is_a<bmi> ())
        {
          seen_obj = seen_obj || true;
        }
        else if (p.is_a<obje> () || p.is_a<bmie> ())
        {
          if (ot != otype::e)
            fail << p.type ().name << "{} as prerequisite of " << t;

          seen_obj = seen_obj || true;
        }
        else if (p.is_a<obja> () || p.is_a<bmia> ())
        {
          if (ot != otype::a)
            fail << p.type ().name << "{} as prerequisite of " << t;

          seen_obj = seen_obj || true;
        }
        else if (p.is_a<objs> () || p.is_a<bmis> ())
        {
          if (ot != otype::s)
            fail << p.type ().name << "{} as prerequisite of " << t;

          seen_obj = seen_obj || true;
        }
        else if (p.is_a<libx> ()  ||
                 p.is_a<liba> ()  ||
                 p.is_a<libs> ()  ||
                 p.is_a<libux> ())
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

      return true;
    }

    auto link_rule::
    derive_libs_paths (file& ls, const char* pfx, const char* sfx) const
      -> libs_paths
    {
      const char* ext (nullptr);

      bool win (tclass == "windows");

      if (win)
      {
        if (tsys == "mingw32")
        {
          if (pfx == nullptr)
            pfx = "lib";
        }

        ext = "dll";
      }
      else
      {
        if (pfx == nullptr)
          pfx = "lib";

        if (tclass == "macos")
          ext = "dylib";
        else
          ext = "so";
      }

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
          fail << "no version for " << ctgt << " in bin.lib.version" <<
            info << "considere adding " << tsys << "@<ver> or " << tclass
               << "@<ver>";

        v = i->second;
      }

      // Now determine the paths.
      //
      path lk, so, in;

      // We start with the basic path.
      //
      path b (ls.dir);
      path cp; // Clean pattern.
      {
        if (pfx == nullptr || pfx[0] == '\0')
        {
          b /= ls.name;
        }
        else
        {
          b /= pfx;
          b += ls.name;
        }

        cp = b;
        cp += "?*"; // Don't match empty (like the libfoo.so symlink).

        if (sfx != nullptr)
        {
          b += sfx;
          cp += sfx;
        }
      }

      append_ext (cp);

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

        libi& li (ls.member->as<libi> ()); // Note: libi is locked.
        lk = li.derive_path (move (lk), tsys == "mingw32" ? "a" : "lib");
      }
      else if (!v.empty ())
      {
        lk = b;
        append_ext (lk);
      }

      if (!v.empty ())
        b += v;

      const path& re (ls.derive_path (move (b)));

      return libs_paths {move (lk), move (so), move (in), &re, move (cp)};
    }

    recipe link_rule::
    apply (action a, target& xt) const
    {
      tracer trace (x, "link_rule::apply");

      file& t (xt.as<file> ());

      // Note that for-install is signalled by install_rule and therefore
      // can only be relied upon during execute.
      //
      match_data& md (t.data (match_data ()));

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      ltype lt (link_type (t));
      otype ot (lt.type);
      linfo li (link_info (bs, ot));

      // Set the library type (C, C++, etc).
      //
      if (lt.library ())
        t.vars.assign (c_type) = string (x);

      // Derive file name(s) and add ad hoc group members.
      //
      {
        target_lock libi; // Have to hold until after PDB member addition.

        const char* e (nullptr); // Extension.
        const char* p (nullptr); // Prefix.
        const char* s (nullptr); // Suffix.

        if (lt.utility)
        {
          // These are all static libraries with names indicating the kind of
          // object files they contain (similar to how we name object files
          // themselves). We add the 'u' extension to avoid clashes with
          // real libraries/import stubs.
          //
          // libue  libhello.u.a     hello.exe.u.lib
          // libua  libhello.a.u.a   hello.lib.u.lib
          // libus  libhello.so.u.a  hello.dll.u.lib  hello.dylib.u.lib
          //
          // Note that we currently don't add bin.lib.{prefix,suffix} since
          // these are not installed.
          //
          if (tsys == "win32-msvc")
          {
            switch (ot)
            {
            case otype::e: e = "exe.u.lib"; break;
            case otype::a: e = "lib.u.lib"; break;
            case otype::s: e = "dll.u.lib"; break;
            }
          }
          else
          {
            p = "lib";

            if (tsys == "mingw32")
            {
              switch (ot)
              {
              case otype::e: e = "exe.u.a"; break;
              case otype::a: e = "a.u.a";   break;
              case otype::s: e = "dll.u.a"; break;
              }

            }
            else if (tsys == "darwin")
            {
              switch (ot)
              {
              case otype::e: e = "u.a";       break;
              case otype::a: e = "a.u.a";     break;
              case otype::s: e = "dylib.u.a"; break;
              }
            }
            else
            {
              switch (ot)
              {
              case otype::e: e = "u.a";    break;
              case otype::a: e = "a.u.a";  break;
              case otype::s: e = "so.u.a"; break;
              }
            }
          }

          t.derive_path (e, p, s);
        }
        else
        {
          if (auto l = t[ot == otype::e ? "bin.exe.prefix" : "bin.lib.prefix"])
            p = cast<string> (l).c_str ();
          if (auto l = t[ot == otype::e ? "bin.exe.suffix" : "bin.lib.suffix"])
            s = cast<string> (l).c_str ();

          switch (ot)
          {
          case otype::e:
            {
              if (tclass == "windows")
                e = "exe";
              else
                e = "";

              t.derive_path (e, p, s);
              break;
            }
          case otype::a:
            {
              if (tsys == "win32-msvc")
                e = "lib";
              else
              {
                if (p == nullptr) p = "lib";
                e = "a";
              }

              t.derive_path (e, p, s);
              break;
            }
          case otype::s:
            {
              // On Windows libs{} is an ad hoc group. The libs{} itself is
              // the DLL and we add libi{} import library as its member.
              //
              if (tclass == "windows")
                libi = add_adhoc_member<bin::libi> (a, t);

              md.libs_data = derive_libs_paths (t, p, s);

              if (libi)
                match_recipe (libi, group_recipe); // Set recipe and unlock.

              break;
            }
          }

          // Add VC's .pdb. Note that we are looking for the link.exe /DEBUG
          // option.
          //
          if (ot != otype::a                               &&
              tsys == "win32-msvc"                         &&
              (find_option ("/DEBUG", t, c_loptions, true) ||
               find_option ("/DEBUG", t, x_loptions, true)))
          {
            // Note: add after the import library if any.
            //
            target_lock pdb (
              add_adhoc_member (a, t, *bs.find_target_type ("pdb")));

            // We call it foo.{exe,dll}.pdb rather than just foo.pdb because
            // we can have both foo.exe and foo.dll in the same directory.
            //
            pdb.target->as<file> ().derive_path (t.path (), "pdb");

            match_recipe (pdb, group_recipe); // Set recipe and unlock.
          }

          // Add pkg-config's .pc file.
          //
          // Note that we do it here regardless of whether we are installing
          // or not for two reasons. Firstly, it is not easy to detect this
          // situation in apply() since the for-install hasn't yet been
          // communicated by install_rule. Secondly, always having the member
          // takes care of cleanup automagically. The actual generation
          // happens in perform_update() below.
          //
          if (ot != otype::e)
          {
            target_lock pc (
              add_adhoc_member (
                a, t,
                ot == otype::a ? pca::static_type : pcs::static_type));

            // Note that here we always use the lib name prefix, even on
            // Windows with VC. The reason is the user needs a consistent name
            // across platforms by which they can refere to the library. This
            // is also the reason why we use the static/shared suffixes rather
            // that a./.lib/.so/.dylib/.dll.
            //
            pc.target->as<file> ().derive_path (nullptr,
                                                (p == nullptr ? "lib" : p),
                                                s);

            match_recipe (pc, group_recipe); // Set recipe and unlock.
          }

          // Add the Windows rpath emulating assembly directory as fsdir{}.
          //
          // Currently this is used in the backlinking logic and in the future
          // could also be used for clean (though there we may want to clean
          // old assemblies).
          //
          if (ot == otype::e && tclass == "windows")
          {
            // Note that here we cannot determine whether we will actually
            // need one (for_install, library timestamps are not available at
            // this point to call windows_rpath_timestamp()). So we may add
            // the ad hoc target but actually not produce the assembly. So
            // whomever relies on this must check if the directory actually
            // exists (windows_rpath_assembly() does take care to clean it up
            // if not used).
            //
            target_lock dir (
              add_adhoc_member (
                a,
                t,
                fsdir::static_type,
                path_cast<dir_path> (t.path () + ".dlls"),
                t.out,
                string ()));

            // By default our backlinking logic will try to symlink the
            // directory and it can even be done on Windows using junctions.
            // The problem is the Windows DLL assembly "logic" refuses to
            // recognize a junction as a valid assembly for some reason. So we
            // are going to resort to copy-link (i.e., a real directory with a
            // bunch on links).
            //
            // Interestingly, the directory symlink works just fine under
            // Wine. So we only resort to copy-link'ing if we are running
            // on Windows.
            //
#ifdef _WIN32
            dir.target->assign (var_backlink) = "copy";
#endif
            match_recipe (dir, group_recipe); // Set recipe and unlock.
          }
        }
      }

      // Inject dependency on the output directory.
      //
      inject_fsdir (a, t);

      // Process prerequisites, pass 1: search and match prerequisite
      // libraries, search obj/bmi{} targets, and search targets we do rule
      // chaining for.
      //
      // We do libraries first in order to indicate that we will execute these
      // targets before matching any of the obj/bmi{}. This makes it safe for
      // compile::apply() to unmatch them and therefore not to hinder
      // parallelism.
      //
      // We also create obj/bmi{} chain targets because we need to add
      // (similar to lib{}) all the bmi{} as prerequisites to all the other
      // obj/bmi{} that we are creating. Note that this doesn't mean that the
      // compile rule will actually treat them all as prerequisite targets.
      // Rather, they are used to resolve actual module imports. We don't
      // really have to search obj{} targets here but it's the same code so we
      // do it here to avoid duplication.
      //
      // Also, when cleaning, we ignore prerequisites that are not in the same
      // or a subdirectory of our project root.
      //
      optional<dir_paths> usr_lib_dirs; // Extract lazily.
      compile_target_types tt (compile_types (ot));

      auto skip = [&a, &rs] (const target*& pt)
      {
        if (a.operation () == clean_id && !pt->dir.sub (rs.out_path ()))
          pt = nullptr;

        return pt == nullptr;
      };

      auto& pts (t.prerequisite_targets[a]);
      size_t start (pts.size ());

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // We pre-allocate a NULL slot for each (potential; see clean)
        // prerequisite target.
        //
        pts.push_back (nullptr);
        const target*& pt (pts.back ());

        uint8_t m (0); // Mark: lib (0), src (1), mod (2), obj/bmi (3).

        bool mod (x_mod != nullptr && p.is_a (*x_mod));

        if (mod || p.is_a (x_src) || p.is_a<c> ())
        {
          // Rule chaining, part 1.
          //

          // Which scope shall we use to resolve the root? Unlikely, but
          // possible, the prerequisite is from a different project
          // altogether. So we are going to use the target's project.
          //

          // If the source came from the lib{} group, then create the obj{}
          // group and add the source as a prerequisite of the obj{} group,
          // not the obj*{} member. This way we only need one prerequisite
          // for, say, both liba{} and libs{}. The same goes for bmi{}.
          //
          bool group (!p.prerequisite.belongs (t)); // Group's prerequisite.

          const target_type& rtt (mod
                                  ? (group ? bmi::static_type : tt.bmi)
                                  : (group ? obj::static_type : tt.obj));

          const prerequisite_key& cp (p.key ()); // Source key.

          // Come up with the obj*/bmi*{} target. The source prerequisite
          // directory can be relative (to the scope) or absolute. If it is
          // relative, then use it as is. If absolute, then translate it to
          // the corresponding directory under out_root. While the source
          // directory is most likely under src_root, it is also possible it
          // is under out_root (e.g., generated source).
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
                  info << "specify corresponding " << rtt.name << "{} "
                     << "target explicitly";

              d = rs.out_path () / cpd.leaf (rs.src_path ());
            }
          }

          // obj/bmi{} is always in the out tree. Note that currently it could
          // be the group -- we will pick a member in part 2 below.
          //
          pt = &search (t, rtt, d, dir_path (), *cp.tk.name, nullptr, cp.scope);

          // If we shouldn't clean obj{}, then it is fair to assume we
          // shouldn't clean the source either (generated source will be in
          // the same directory as obj{} and if not, well, go find yourself
          // another build system ;-)).
          //
          if (skip (pt))
            continue;

          m = mod ? 2 : 1;
        }
        else if (p.is_a<libx> () ||
                 p.is_a<liba> () ||
                 p.is_a<libs> () ||
                 p.is_a<libux> ())
        {
          // Handle imported libraries.
          //
          // Note that since the search is rule-specific, we don't cache the
          // target in the prerequisite.
          //
          if (p.proj ())
            pt = search_library (
              a, sys_lib_dirs, usr_lib_dirs, p.prerequisite);

          // The rest is the same basic logic as in search_and_match().
          //
          if (pt == nullptr)
            pt = &p.search (t);

          if (skip (pt))
            continue;

          // If this is the lib{}/libu{} group, then pick the appropriate
          // member.
          //
          if (const libx* l = pt->is_a<libx> ())
            pt = &link_member (*l, a, li);
        }
        else
        {
          // If this is the obj{} or bmi{} target group, then pick the
          // appropriate member.
          //
          if      (p.is_a<obj> ()) pt = &search (t, tt.obj, p.key ());
          else if (p.is_a<bmi> ()) pt = &search (t, tt.bmi, p.key ());
          //
          // Something else. This could be something unrelated that the user
          // tacked on (e.g., a doc{}). Or it could be some ad hoc input to
          // the linker (say a linker script or some such).
          //
          else
          {
            if (!p.is_a<objx> () && !p.is_a<bmix> ())
            {
              // @@ Temporary hack until we get the default outer operation
              // for update. This allows operations like test and install to
              // skip such tacked on stuff.
              //
              if (current_outer_oif != nullptr)
                continue;
            }

            pt = &p.search (t);
          }

          if (skip (pt))
            continue;

          m = 3;
        }

        mark (pt, m);
      }

      // Match lib{} (the only unmarked) in parallel and wait for completion.
      //
      match_members (a, t, pts, start);

      // Process prerequisites, pass 2: finish rule chaining but don't start
      // matching anything yet since that may trigger recursive matching of
      // bmi{} targets we haven't completed yet. Hairy, I know.
      //

      // Parallel prerequisite_targets loop.
      //
      size_t i (start), n (pts.size ());
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        const target*& pt (pts[i].target);
        uintptr_t&     pd (pts[i++].data);

        if (pt == nullptr)
          continue;

        uint8_t m (unmark (pt)); // New mark: completion (1), verfication (2).

        if (m == 3)                // obj/bmi{}
          m = 1;                   // Just completion.
        else if (m == 1 || m == 2) // Source/module chain.
        {
          bool mod (m == 2);

          m = 1;

          const target& rt (*pt);
          bool group (!p.prerequisite.belongs (t)); // Group's prerequisite.

          // If we have created a obj/bmi{} target group, pick one of its
          // members; the rest would be primarily concerned with it.
          //
          pt =
            group
            ? &search (t, (mod ? tt.bmi : tt.obj), rt.dir, rt.out, rt.name)
            : &rt;

          // If this obj*{} already has prerequisites, then verify they are
          // "compatible" with what we are doing here. Otherwise, synthesize
          // the dependency. Note that we may also end up synthesizing with
          // someone beating us to it. In this case also verify.
          //
          bool verify (true);

          if (!pt->has_prerequisites ())
          {
            prerequisites ps;
            ps.push_back (p.as_prerequisite ()); // Source.

            // Add our lib*{} (see the export.* machinery for details) and
            // bmi*{} (both original and chained; see module search logic)
            // prerequisites.
            //
            // Note that we don't resolve lib{} to liba{}/libs{} here
            // instead leaving it to whomever (e.g., the compile rule) will
            // be needing *.export.*. One reason for doing it there is that
            // the object target might be specified explicitly by the user
            // in which case they will have to specify the set of lib{}
            // prerequisites and it's much cleaner to do as lib{} rather
            // than liba{}/libs{}.
            //
            // Initially, we were only adding imported libraries, but there
            // is a problem with this approach: the non-imported library
            // might depend on the imported one(s) which we will never "see"
            // unless we start with this library.
            //
            // Note: have similar logic in make_module_sidebuild().
            //
            size_t j (start);
            for (prerequisite_member p: group_prerequisite_members (a, t))
            {
              const target* pt (pts[j++]);

              if (pt == nullptr)
                continue;

              if (p.is_a<libx> () ||
                  p.is_a<liba> () || p.is_a<libs> () || p.is_a<libux> () ||
                  p.is_a<bmi> ()  || p.is_a (tt.bmi))
              {
                ps.push_back (p.as_prerequisite ());
              }
              else if (x_mod != nullptr && p.is_a (*x_mod)) // Chained module.
              {
                // Searched during pass 1 but can be NULL or marked.
                //
                if (pt != nullptr && i != j) // Don't add self (note: both +1).
                {
                  // This is sticky: pt might have come before us and if it
                  // was a group, then we would have picked up a member. So
                  // here we may have to "unpick" it.
                  //
                  bool group (j < i && !p.prerequisite.belongs (t));

                  unmark (pt);
                  ps.push_back (prerequisite (group ? *pt->group : *pt));
                }
              }
            }

            // Note: add to the group, not the member.
            //
            verify = !rt.prerequisites (move (ps));
          }

          if (verify)
          {
            // This gets a bit tricky. We need to make sure the source files
            // are the same which we can only do by comparing the targets to
            // which they resolve. But we cannot search ot's prerequisites --
            // only the rule that matches can. Note, however, that if all this
            // works out, then our next step is to match the obj*{} target. If
            // things don't work out, then we fail, in which case searching
            // and matching speculatively doesn't really hurt. So we start the
            // async match here and finish this verification in the "harvest"
            // loop below.
            //
            const target_type& rtt (mod
                                    ? (group ? bmi::static_type : tt.bmi)
                                    : (group ? obj::static_type : tt.obj));

            resolve_group (a, *pt); // Not matched yet so resolve group.

            bool src (false);
            for (prerequisite_member p1: group_prerequisite_members (a, *pt))
            {
              // Most of the time we will have just a single source so fast-
              // path that case.
              //
              if (p1.is_a (mod ? *x_mod : x_src) || p1.is_a<c> ())
              {
                src = true;
                continue; // Check the rest of the prerequisites.
              }

              // Ignore some known target types (fsdir, headers, libraries,
              // modules).
              //
              if (p1.is_a<fsdir> ()                                         ||
                  p1.is_a<libx>  ()                                         ||
                  p1.is_a<liba> () || p1.is_a<libs> () || p1.is_a<libux> () ||
                  p1.is_a<bmi>  () || p1.is_a<bmix> ()                      ||
                  (p.is_a (mod ? *x_mod : x_src) && x_header (p1))          ||
                  (p.is_a<c> () && p1.is_a<h> ()))
                continue;

              fail << "synthesized dependency for prerequisite " << p
                   << " would be incompatible with existing target " << *pt <<
                info << "unexpected existing prerequisite type " << p1 <<
                info << "specify corresponding " << rtt.name << "{} "
                   << "dependency explicitly";
            }

            if (!src)
              fail << "synthesized dependency for prerequisite " << p
                   << " would be incompatible with existing target " << *pt <<
                info << "no existing c/" << x_name << " source prerequisite" <<
                info << "specify corresponding " << rtt.name << "{} "
                   << "dependency explicitly";

            m = 2; // Needs verification.
          }
        }
        else // lib*{}
        {
          // If this is a static library, see if we need to link it whole.
          // Note that we have to do it after match since we rely on the
          // group link-up.
          //
          bool u;
          if ((u = pt->is_a<libux> ()) || pt->is_a<liba> ())
          {
            const variable& var (var_pool["bin.whole"]); // @@ Cache.

            // See the bin module for the lookup semantics discussion. Note
            // that the variable is not overridable so we omit find_override()
            // calls.
            //
            lookup l (p.prerequisite.vars[var]);

            if (!l.defined ())
              l = pt->find_original (var, true).first;

            if (!l.defined ())
            {
              bool g (pt->group != nullptr);
              l = bs.find_original (var,
                                    &pt->type (),
                                    &pt->name,
                                    (g ? &pt->group->type () : nullptr),
                                    (g ? &pt->group->name : nullptr)).first;
            }

            if (l ? cast<bool> (*l) : u)
              pd |= lflag_whole;
          }
        }

        mark (pt, m);
      }

      // Process prerequisites, pass 3: match everything and verify chains.
      //

      // Wait with unlocked phase to allow phase switching.
      //
      wait_guard wg (target::count_busy (), t[a].task_count, true);

      for (i = start; i != n; ++i)
      {
        const target*& pt (pts[i]);

        if (pt == nullptr)
          continue;

        if (uint8_t m = unmark (pt))
        {
          match_async (a, *pt, target::count_busy (), t[a].task_count);
          mark (pt, m);
        }
      }

      wg.wait ();

      // The "harvest" loop: finish matching the targets we have started. Note
      // that we may have bailed out early (thus the parallel i/n for-loop).
      //
      i = start;
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        const target*& pt (pts[i++]);

        // Skipped or not marked for completion.
        //
        uint8_t m;
        if (pt == nullptr || (m = unmark (pt)) == 0)
          continue;

        build2::match (a, *pt);

        // Nothing else to do if not marked for verification.
        //
        if (m == 1)
          continue;

        // Finish verifying the existing dependency (which is now matched)
        // compared to what we would have synthesized.
        //
        bool mod (x_mod != nullptr && p.is_a (*x_mod));

        // Note: group already resolved in the previous loop.

        for (prerequisite_member p1: group_prerequisite_members (a, *pt))
        {
          if (p1.is_a (mod ? *x_mod : x_src) || p1.is_a<c> ())
          {
            // Searching our own prerequisite is ok, p1 must already be
            // resolved.
            //
            if (&p.search (t) != &p1.search (*pt))
            {
              bool group (!p.prerequisite.belongs (t));

              const target_type& rtt (mod
                                      ? (group ? bmi::static_type : tt.bmi)
                                      : (group ? obj::static_type : tt.obj));

              fail << "synthesized dependency for prerequisite " << p << " "
                   << "would be incompatible with existing target " << *pt <<
                info << "existing prerequisite " << p1 << " does not match "
                   << p <<
                info << "specify corresponding " << rtt.name << "{} "
                   << "dependency explicitly";
            }

            break;
          }
        }
      }

      switch (a)
      {
      case perform_update_id: return [this] (action a, const target& t)
        {
          return perform_update (a, t);
        };
      case perform_clean_id: return [this] (action a, const target& t)
        {
          return perform_clean (a, t);
        };
      default: return noop_recipe; // Configure update.
      }
    }

    void link_rule::
    append_libraries (strings& args,
                      const file& l, bool la, lflags lf,
                      const scope& bs, action a, linfo li) const
    {
      // Note: lack of the "small function object" optimization will really
      // kill us here since we are called in a loop.
      //
      auto imp = [] (const file&, bool la) {return la;};

      auto lib = [&args, this] (const file* l, const string& p, lflags f, bool)
      {
        if (l != nullptr)
        {
          // On Windows a shared library is a DLL with the import library as a
          // first ad hoc group member. MinGW though can link directly to DLLs
          // (see search_library() for details).
          //
          if (l->member != nullptr && l->is_a<libs> () && tclass == "windows")
            l = &l->member->as<file> ();

          string p (relative (l->path ()).string ());

          if (f & lflag_whole)
          {
            if (tsys == "win32-msvc")
            {
              p.insert (0, "/WHOLEARCHIVE:"); // Only available from VC14U2.
            }
            else if (tsys == "darwin")
            {
              p.insert (0, "-Wl,-force_load,");
            }
            else
            {
              args.push_back ("-Wl,--whole-archive");
              args.push_back (move (p));
              args.push_back ("-Wl,--no-whole-archive");
              return;
            }
          }

          args.push_back (move (p));
        }
        else
          args.push_back (p);
      };

      auto opt = [&args, this] (
        const file& l, const string& t, bool com, bool exp)
      {
        // If we need an interface value, then use the group (lib{}).
        //
        if (const target* g = exp && l.is_a<libs> () ? l.group : &l)
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

      process_libraries (
        a, bs, li, sys_lib_dirs, l, la, lf, imp, lib, opt, true);
    }

    void link_rule::
    hash_libraries (sha256& cs,
                    bool& update, timestamp mt,
                    const file& l, bool la, lflags lf,
                    const scope& bs, action a, linfo li) const
    {
      auto imp = [] (const file&, bool la) {return la;};

      struct data
      {
        sha256&         cs;
        const dir_path& out_root;
        bool&           update;
        timestamp       mt;
      } d {cs, bs.root_scope ()->out_path (), update, mt};

      auto lib = [&d, this] (const file* l, const string& p, lflags f, bool)
      {
        if (l != nullptr)
        {
          // Check if this library renders us out of date.
          //
          d.update = d.update || l->newer (d.mt);

          // On Windows a shared library is a DLL with the import library as a
          // first ad hoc group member. MinGW though can link directly to DLLs
          // (see search_library() for details).
          //
          if (l->member != nullptr && l->is_a<libs> () && tclass == "windows")
            l = &l->member->as<file> ();

          d.cs.append (f);
          hash_path (d.cs, l->path (), d.out_root);
        }
        else
          d.cs.append (p);
      };

      auto opt = [&cs, this] (
        const file& l, const string& t, bool com, bool exp)
      {
        if (const target* g = exp && l.is_a<libs> () ? l.group : &l)
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

      process_libraries (
        a, bs, li, sys_lib_dirs, l, la, lf, imp, lib, opt, true);
    }

    void link_rule::
    rpath_libraries (strings& args,
                     const target& t,
                     const scope& bs,
                     action a,
                     linfo li,
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

      auto imp = [for_install] (const file& l, bool la)
      {
        // If we are not installing, then we only need to rpath interface
        // libraries (they will include rpath's for their implementations)
        // Otherwise, we have to do this recursively. In both cases we also
        // want to see through utility libraries.
        //
        // The rpath-link part is tricky: ideally we would like to get only
        // implementations and only of shared libraries. We are not interested
        // in interfaces because we are linking their libraries explicitly.
        // However, in our model there is no such thing as "implementation
        // only"; it is either interface or interface and implementation. So
        // we are going to rpath-link all of them which should be harmless
        // except for some noise on the command line.
        //
        //
        return (for_install ? !la : false) || l.is_a<libux> ();
      };

      // Package the data to keep within the 2-pointer small std::function
      // optimization limit.
      //
      struct
      {
        strings& args;
        bool for_install;
      } d {args, for_install};

      auto lib = [&d, this] (const file* l, const string& f, lflags, bool sys)
      {
        // We don't rpath system libraries. Why, you may ask? There are many
        // good reasons and I have them written on a napkin somewhere...
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
      const function<bool (const file&, bool)> impf (imp);
      const function<void (const file*, const string&, lflags, bool)> libf (lib);

      for (const prerequisite_target& pt: t.prerequisite_targets[a])
      {
        if (pt == nullptr)
          continue;

        bool la;
        const file* f;

        if ((la = (f = pt->is_a<liba>  ())) ||
            (la = (f = pt->is_a<libux> ())) ||
            (      f = pt->is_a<libs>  ()))
        {
          if (!for_install && !la)
          {
            // Top-level sharen library dependency. It is either matched or
            // imported so should be a cc library.
            //
            if (!cast_false<bool> (f->vars[c_system]))
              args.push_back (
                "-Wl,-rpath," + f->path ().directory ().string ());
          }

          process_libraries (a, bs, li, sys_lib_dirs,
                             *f, la, pt.data,
                             impf, libf, nullptr);
        }
      }
    }

    // Filter link.exe noise (msvc.cxx).
    //
    void
    msvc_filter_link (ifdstream&, const file&, otype);

    // Translate target CPU to the link.exe/lib.exe /MACHINE option.
    //
    const char*
    msvc_machine (const string& cpu); // msvc.cxx

    target_state link_rule::
    perform_update (action a, const target& xt) const
    {
      tracer trace (x, "link_rule::perform_update");

      const file& t (xt.as<file> ());
      const path& tp (t.path ());

      match_data& md (t.data<match_data> ());

      // Unless the outer install rule signalled that this is update for
      // install, signal back that we've performed plain update.
      //
      if (!md.for_install)
        md.for_install = false;

      bool for_install (*md.for_install);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      ltype lt (link_type (t));
      otype ot (lt.type);
      linfo li (link_info (bs, ot));

      // Update prerequisites. We determine if any relevant ones render us
      // out-of-date manually below.
      //
      bool update (false);
      timestamp mt (t.load_mtime ());
      target_state ts (straight_execute_prerequisites (a, t));

      // If targeting Windows, take care of the manifest.
      //
      path manifest; // Manifest itself (msvc) or compiled object file.
      timestamp rpath_timestamp = timestamp_nonexistent; // DLLs timestamp.

      if (lt.executable () && tclass == "windows")
      {
        // First determine if we need to add our rpath emulating assembly. The
        // assembly itself is generated later, after updating the target. Omit
        // it if we are updating for install.
        //
        if (!for_install)
          rpath_timestamp = windows_rpath_timestamp (t, bs, a, li);

        auto p (windows_manifest (t, rpath_timestamp != timestamp_nonexistent));
        path& mf (p.first);
        bool mf_cf (p.second); // Changed flag (timestamp resolution).

        timestamp mf_mt (file_mtime (mf));

        if (tsys == "mingw32")
        {
          // Compile the manifest into the object file with windres. While we
          // are going to synthesize an .rc file to pipe to windres' stdin, we
          // will still use .manifest to check if everything is up-to-date.
          //
          manifest = mf + ".o";

          if (mf_mt > file_mtime (manifest) || mf_cf)
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
                ofdstream os (move (pr.out_fd));

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
              }
              catch (const io_error& e)
              {
                if (pr.wait ()) // Ignore if child failed.
                  fail << "unable to pipe resource file to " << args[0]
                       << ": " << e;
              }

              run_finish (args, pr);
            }
            catch (const process_error& e)
            {
              error << "unable to execute " << args[0] << ": " << e;

              if (e.child)
                exit (1);

              throw failed ();
            }

            update = true; // Manifest changed, force update.
          }
        }
        else
        {
          manifest = move (mf); // Save for link.exe's /MANIFESTINPUT.

          if (mf_mt > mt || mf_cf)
            update = true; // Manifest changed, force update.
        }
      }

      // Check/update the dependency database.
      //
      depdb dd (tp + ".d");

      // First should come the rule name/version.
      //
      if (dd.expect (rule_id) != nullptr)
        l4 ([&]{trace << "rule mismatch forcing update of " << t;});

      lookup ranlib;

      // Then the linker checksum (ar/ranlib or the compiler).
      //
      if (lt.static_library ())
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
            rs[tsys == "win32-msvc"
               ? var_pool["bin.ld.checksum"]
               : x_checksum]));

        if (dd.expect (cs) != nullptr)
          l4 ([&]{trace << "linker mismatch forcing update of " << t;});
      }

      // Next check the target. While it might be incorporated into the linker
      // checksum, it also might not (e.g., VC link.exe).
      //
      if (dd.expect (ctgt.string ()) != nullptr)
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

      // Storage.
      //
      string soname1, soname2;
      strings sargs;

      if (lt.static_library ())
      {
        if (tsys == "win32-msvc") ;
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
        if (tsys == "win32-msvc")
        {
          // We are using link.exe directly so don't pass the compiler
          // options.
        }
        else
        {
          append_options (args, t, c_coptions);
          append_options (args, t, x_coptions);
          append_options (args, tstd);
        }

        append_options (args, t, c_loptions);
        append_options (args, t, x_loptions);

        // Extra system library dirs (last).
        //
        // @@ /LIBPATH:<path>, not /LIBPATH <path>
        //
        assert (sys_lib_dirs_extra <= sys_lib_dirs.size ());
        append_option_values (
          args,
          cclass == compiler_class::msvc ? "/LIBPATH:" : "-L",
          sys_lib_dirs.begin () + sys_lib_dirs_extra, sys_lib_dirs.end (),
          [] (const dir_path& d) {return d.string ().c_str ();});

        // Handle soname/rpath.
        //
        if (tclass == "windows")
        {
          // Limited emulation for Windows with no support for user-defined
          // rpaths.
          //
          auto l (t["bin.rpath"]);

          if (l && !l->empty ())
            fail << ctgt << " does not support rpath";
        }
        else
        {
          // Set soname.
          //
          if (lt.shared_library ())
          {
            const libs_paths& paths (md.libs_data);
            const string& leaf (paths.effect_soname ().leaf ().string ());

            if (tclass == "macos")
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
          rpath_libraries (sargs, t, bs, a, li, for_install);

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

        for (const prerequisite_target& p: t.prerequisite_targets[a])
        {
          const target* pt (p.target);

          if (pt == nullptr)
            continue;

          // If this is bmi*{}, then obj*{} is its ad hoc member.
          //
          if (modules)
          {
            if (pt->is_a<bmix> ())
              pt = pt->member;
          }

          const file* f;
          bool la (false), ls (false);

          if ((f = pt->is_a<objx> ()) ||
              (!lt.static_library () && // @@ UTL: TODO libua to liba link.
               ((la = (f = pt->is_a<liba>  ())) ||
                (la = (f = pt->is_a<libux> ())) ||
                (ls = (f = pt->is_a<libs>  ())))))
          {
            // Link all the dependent interface libraries (shared) or interface
            // and implementation (static), recursively.
            //
            // Also check if any of them render us out of date. The tricky
            // case is, say, a utility library (static) that depends on a
            // shared library. When the shared library is updated, there is no
            // reason to re-archive the utility but those who link the utility
            // have to "see through" the changes in the shared library.
            //
            if (la || ls)
            {
              hash_libraries (cs, update, mt, *f, la, p.data, bs, a, li);
              f = nullptr; // Timestamp checked by hash_libraries().
            }
            else
              hash_path (cs, f->path (), rs.out_path ());
          }
          else
            f = pt->is_a<exe> (); // Consider executable mtime (e.g., linker).

          // Check if this input renders us out of date.
          //
          if (f != nullptr)
            update = update || f->newer (mt);
        }

        // Treat it as input for both MinGW and VC (mtime checked above).
        //
        if (!manifest.empty ())
          hash_path (cs, manifest, rs.out_path ());

        // Treat .libs as inputs, not options.
        //
        if (!lt.static_library ())
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
      if (dd.writing () || dd.mtime () > mt)
        scratch = update = true;

      dd.close ();

      // (Re)generate pkg-config's .pc file. While the target itself might be
      // up-to-date from a previous run, there is no guarantee that .pc exists
      // or also up-to-date. So to keep things simple we just regenerate it
      // unconditionally.
      //
      // Also, if you are wondering why don't we just always produce this .pc,
      // install or no install, the reason is unless and until we are updating
      // for install, we have no idea where-to things will be installed.
      //
      if (for_install)
      {
        bool la;
        const file* f;

        if ((la = (f = t.is_a<liba> ())) ||
            (      f = t.is_a<libs> ()))
          pkgconfig_save (a, *f, la);
      }

      // If nothing changed, then we are done.
      //
      if (!update)
        return ts;

      // Ok, so we are updating. Finish building the command line.
      //
      string out, out1, out2, out3; // Storage.

      // Translate paths to relative (to working directory) ones. This results
      // in easier to read diagnostics.
      //
      path relt (relative (tp));

      const process_path* ld (nullptr);
      if (lt.static_library ())
      {
        ld = &cast<process_path> (rs["bin.ar.path"]);

        if (tsys == "win32-msvc")
        {
          // lib.exe has /LIBPATH but it's not clear/documented what it's used
          // for. Perhaps for link-time code generation (/LTCG)? If that's the
          // case, then we may need to pass *.loptions.
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
      }
      else
      {
        // The options are usually similar enough to handle executables
        // and shared libraries together.
        //
        if (tsys == "win32-msvc")
        {
          // Using link.exe directly.
          //
          ld = &cast<process_path> (rs["bin.ld.path"]);
          args.push_back ("/NOLOGO");

          if (ot == otype::s)
            args.push_back ("/DLL");

          // Add /MACHINE.
          //
          args.push_back (msvc_machine (cast<string> (rs[x_target_cpu])));

          // Unless explicitly enabled with /INCREMENTAL, disable incremental
          // linking (it is implicitly enabled if /DEBUG is specified). The
          // reason is the .ilk file: its name cannot be changed and if we
          // have, say, foo.exe and foo.dll, then they will end up stomping on
          // each other's .ilk's.
          //
          // So the idea is to disable it by default but let the user request
          // it explicitly if they are sure their project doesn't suffer from
          // the above issue. We can also have something like 'incremental'
          // config initializer keyword for this.
          //
          // It might also be a good idea to ask Microsoft to add an option.
          //
          if (!find_option ("/INCREMENTAL", args, true))
            args.push_back ("/INCREMENTAL:NO");

          if (cid == compiler_id::clang)
          {
            // According to Clang's MSVC.cpp, we shall link libcmt.lib (static
            // multi-threaded runtime) unless -nostdlib or -nostartfiles is
            // specified.
            //
            if (!find_options ({"-nostdlib", "-nostartfiles"}, t, c_coptions) &&
                !find_options ({"-nostdlib", "-nostartfiles"}, t, x_coptions))
              args.push_back ("/DEFAULTLIB:libcmt.lib");
          }

          // If you look at the list of libraries Visual Studio links by
          // default, it includes everything and a couple of kitchen sinks
          // (winspool32.lib, ole32.lib, odbc32.lib, etc) while we want to
          // keep our low-level build as pure as possible. However, there seem
          // to be fairly essential libraries that are not linked by link.exe
          // by default (use /VERBOSE:LIB to see the list). For example, MinGW
          // by default links advapi32, shell32, user32, and kernel32. And so
          // we follow suit and make sure those are linked.  advapi32 and
          // kernel32 are already on the default list and we only need to add
          // the other two.
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

          if (ot == otype::s)
          {
            // On Windows libs{} is the DLL and its first ad hoc group member
            // is the import library.
            //
            // This will also create the .exp export file. Its name will be
            // derived from the import library by changing the extension.
            // Lucky for us -- there is no option to name it.
            //
            auto& imp (t.member->as<file> ());
            out2 = "/IMPLIB:" + relative (imp.path ()).string ();
            args.push_back (out2.c_str ());
          }

          // If we have /DEBUG then name the .pdb file. It is either the first
          // (exe) or the second (dll) ad hoc group member.
          //
          if (find_option ("/DEBUG", args, true))
          {
            auto& pdb (
              (ot == otype::e ? t.member : t.member->member)->as<file> ());
            out1 = "/PDB:" + relative (pdb.path ()).string ();
            args.push_back (out1.c_str ());
          }

          // @@ An executable can have an import library and VS seems to
          //    always name it. I wonder what would trigger its generation?
          //    Could it be the presence of export symbols? Yes, link.exe will
          //    generate the import library iff there are exported symbols.
          //    Which means there could be a DLL without an import library
          //    (which we currently don't handle very well).
          //
          out = "/OUT:" + relt.string ();
          args.push_back (out.c_str ());
        }
        else
        {
          switch (cclass)
          {
          case compiler_class::gcc:
            {
              ld = &cpath;

              // Add the option that triggers building a shared library and
              // take care of any extras (e.g., import library).
              //
              if (ot == otype::s)
              {
                if (tclass == "macos")
                  args.push_back ("-dynamiclib");
                else
                  args.push_back ("-shared");

                if (tsys == "mingw32")
                {
                  // On Windows libs{} is the DLL and its first ad hoc group
                  // member is the import library.
                  //
                  auto& imp (t.member->as<file> ());
                  out = "-Wl,--out-implib=" + relative (imp.path ()).string ();
                  args.push_back (out.c_str ());
                }
              }

              args.push_back ("-o");
              args.push_back (relt.string ().c_str ());
              break;
            }
          case compiler_class::msvc: assert (false);
          }
        }
      }

      args[0] = ld->recall_string ();

      // The same logic as during hashing above.
      //
      for (const prerequisite_target& p: t.prerequisite_targets[a])
      {
        const target* pt (p.target);

        if (pt == nullptr)
          continue;

        if (modules)
        {
          if (pt->is_a<bmix> ())
            pt = pt->member;
        }

        const file* f;
        bool la (false), ls (false);

        if ((f = pt->is_a<objx> ()) ||
            (!lt.static_library () && // @@ UTL: TODO libua to liba link.
             ((la = (f = pt->is_a<liba>  ())) ||
              (la = (f = pt->is_a<libux> ())) ||
              (ls = (f = pt->is_a<libs>  ())))))
        {
          // Link all the dependent interface libraries (shared) or interface
          // and implementation (static), recursively.
          //
          if (la || ls)
            append_libraries (sargs, *f, la, p.data, bs, a, li);
          else
            sargs.push_back (relative (f->path ()).string ()); // string()&&
        }
      }

      // For MinGW manifest is an object file.
      //
      if (!manifest.empty () && tsys == "mingw32")
        sargs.push_back (relative (manifest).string ());

      // Shallow-copy sargs to args. Why not do it as we go along pushing into
      // sargs? Because of potential reallocations.
      //
      for (const string& a: sargs)
        args.push_back (a.c_str ());

      if (!lt.static_library ())
      {
        append_options (args, t, c_libs);
        append_options (args, t, x_libs);
      }

      args.push_back (nullptr);

      // Cleanup old (versioned) libraries.
      //
      if (lt.shared_library ())
      {
        const libs_paths& paths (md.libs_data);
        const path& p (paths.clean);

        if (!p.empty ())
        try
        {
          if (verb >= 4) // Seeing this with -V doesn't really add any value.
            text << "rm " << p;

          auto rm = [&paths, this] (path&& m, const string&, bool interm)
          {
            if (!interm)
            {
              // Filter out paths that have one of the current paths as a
              // prefix.
              //
              auto test = [&m] (const path& p)
              {
                const string& s (p.string ());
                return s.empty () || m.string ().compare (0, s.size (), s) != 0;
              };

              if (test (*paths.real)  &&
                  test (paths.interm) &&
                  test (paths.soname) &&
                  test (paths.link))
              {
                try_rmfile (m);
                try_rmfile (m + ".d");

                if (tsys == "win32-msvc")
                {
                  try_rmfile (m.base () += ".ilk");
                  try_rmfile (m += ".pdb");
                }
              }
            }
            return true;
          };

          path_search (p,
                       rm,
                       dir_path () /* start */,
                       false /* follow_symlinks */);
        }
        catch (const system_error&) {} // Ignore errors.
      }
      else if (lt.static_library ())
      {
        // We use relative paths to the object files which means we may end
        // up with different ones depending on CWD and some implementation
        // treat them as different archive members. So remote the file to
        // be sure. Note that we ignore errors leaving it to the achiever
        // to complain.
        //
        if (mt != timestamp_nonexistent)
          try_rmfile (relt, true);
      }

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
        bool filter (tsys == "win32-msvc" && !lt.static_library ());

        process pr (*ld, args.data (), 0, (filter ? -1 : 2));

        if (filter)
        {
          try
          {
            ifdstream is (
              move (pr.in_ofd), fdstream_mode::text, ifdstream::badbit);

            msvc_filter_link (is, t, ot);

            // If anything remains in the stream, send it all to stderr. Note
            // that the eof check is important: if the stream is at eof, this
            // and all subsequent writes to the diagnostics stream will fail
            // (and you won't see a thing).
            //
            if (is.peek () != ifdstream::traits_type::eof ())
              diag_stream_lock () << is.rdbuf ();

            is.close ();
          }
          catch (const io_error&) {} // Assume exits with error.
        }

        run_finish (args, pr);
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child)
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

        run (rl, args);
      }

      if (tclass == "windows")
      {
        // For Windows generate (or clean up) rpath-emulating assembly.
        //
        if (lt.executable ())
          windows_rpath_assembly (t, bs, a, li,
                                  cast<string> (rs[x_target_cpu]),
                                  rpath_timestamp,
                                  scratch);
      }
      else if (lt.shared_library ())
      {
        // For shared libraries we may need to create a bunch of symlinks.
        //
        auto ln = [] (const path& f, const path& l)
        {
          if (verb >= 3)
            text << "ln -sf " << f << ' ' << l;

          try
          {
            if (file_exists (l, false /* follow_symlinks */)) // The -f part.
              try_rmfile (l);

            mksymlink (f, l);
          }
          catch (const system_error& e)
          {
            fail << "unable to create symlink " << l << ": " << e;
          }
        };

        const libs_paths& paths (md.libs_data);

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

    target_state link_rule::
    perform_clean (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());
      ltype lt (link_type (t));

      if (lt.executable ())
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
      }
      else if (lt.shared_library ())
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
          const libs_paths& paths (t.data<match_data> ().libs_data);

          return clean_extra (a, t, {".d",
                paths.link.string ().c_str (),
                paths.soname.string ().c_str (),
                paths.interm.string ().c_str ()});
        }
      }
      // For static library it's just the defaults.

      return clean_extra (a, t, {".d"});
    }
  }
}
