// file      : build2/cc/link-rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/link-rule.hxx>

#include <map>
#include <cstdlib>  // exit()
#include <cstring>  // strlen()

#include <libbutl/filesystem.mxx> // file_exists()

#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/target.hxx>  // c, pc*
#include <build2/cc/utility.hxx>

using std::map;
using std::exit;

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

    link_rule::match_result link_rule::
    match (action a,
           const target& t,
           const target* g,
           otype ot,
           bool library) const
    {
      // NOTE: the target may be a group (see utility library logic below).

      match_result r;

      // Scan prerequisites and see if we can work with what we've got. Note
      // that X could be C (as in language). We handle this by always checking
      // for X first.
      //
      // Note also that we treat bmi{} as obj{}. @@ MODHDR hbmi{}?
      //
      for (prerequisite_member p:
             prerequisite_members (a, t, group_prerequisites (t, g)))
      {
        // If excluded or ad hoc, then don't factor it into our tests.
        //
        if (include (a, t, p) != include_type::normal)
          continue;

        if (p.is_a (x_src)                        ||
            (x_mod != nullptr && p.is_a (*x_mod)) ||
            // Header-only X library (or library with C source and X header).
            (library          && x_header (p, false /* c_hdr */)))
        {
          r.seen_x = r.seen_x || true;
        }
        else if (p.is_a<c> ()            ||
                 // Header-only C library.
                 (library && p.is_a<h> ()))
        {
          r.seen_c = r.seen_c || true;
        }
        else if (p.is_a<obj> () || p.is_a<bmi> ())
        {
          r.seen_obj = r.seen_obj || true;
        }
        else if (p.is_a<obje> () || p.is_a<bmie> ())
        {
          // We can make these "no-match" if/when there is a valid use case.
          //
          if (ot != otype::e)
            fail << p.type ().name << "{} as prerequisite of " << t;

          r.seen_obj = r.seen_obj || true;
        }
        else if (p.is_a<obja> () || p.is_a<bmia> ())
        {
          if (ot != otype::a)
            fail << p.type ().name << "{} as prerequisite of " << t;

          r.seen_obj = r.seen_obj || true;
        }
        else if (p.is_a<objs> () || p.is_a<bmis> ())
        {
          if (ot != otype::s)
            fail << p.type ().name << "{} as prerequisite of " << t;

          r.seen_obj = r.seen_obj || true;
        }
        else if (p.is_a<libul> () || p.is_a<libux> ())
        {
          // For a unility library we look at its prerequisites, recursively.
          // Since these checks are not exactly light-weight, only do them if
          // we haven't already seen any X prerequisites.
          //
          if (!r.seen_x)
          {
            // This is a bit iffy: in our model a rule can only search a
            // target's prerequisites if it matches. But we don't yet know
            // whether we match. However, it seems correct to assume that any
            // rule-specific search will always resolve to an existing target
            // if there is one. So perhaps it's time to relax this restriction
            // a little? Note that this fits particularly well with what we
            // doing here since if there is no existing target, then there can
            // be no prerequisites.
            //
            // Note, however, that we cannot linkup a prerequisite target
            // member to its group since we are not matching this target. As
            // result we have to do all the steps except for setting t.group
            // and pass both member and group (we also cannot query t.group
            // since it's racy).
            //
            const target* pg (nullptr);
            const target* pt (p.search_existing ());

            if (p.is_a<libul> ())
            {
              if (pt != nullptr)
              {
                // If this is a group then try to pick (again, if exists) a
                // suitable member. If it doesn't exist, then we will only be
                // considering the group's prerequisites.
                //
                if (const target* pm =
                    link_member (pt->as<libul> (),
                                 a,
                                 linfo {ot, lorder::a /* unused */},
                                 true /* existing */))
                {
                  pg = pt;
                  pt = pm;
                }
              }
              else
              {
                // It's possible we have no group but have a member so try
                // that.
                //
                const target_type& tt (ot == otype::a ? libua::static_type :
                                       ot == otype::s ? libus::static_type :
                                       libue::static_type);

                // We know this prerequisite member is a prerequisite since
                // otherwise the above search would have returned the member
                // target.
                //
                pt = search_existing (p.prerequisite.key (tt));
              }
            }
            else if (!p.is_a<libue> ())
            {
              // See if we also/instead have a group.
              //
              pg = search_existing (p.prerequisite.key (libul::static_type));

              if (pt == nullptr)
                swap (pt, pg);
            }

            if (pt != nullptr)
            {
              // If we are matching a target, use the original output type
              // since that would be the member that we pick.
              //
              otype pot (pt->is_a<libul> () ? ot : link_type (*pt).type);
              match_result pr (match (a, *pt, pg, pot, true /* lib */));

              // Do we need to propagate any other seen_* values? Hm, that
              // would in fact match with the "see-through" semantics of
              // utility libraries we have in other places.
              //
              r.seen_x = pr.seen_x;
            }
            else
              r.seen_lib = r.seen_lib || true; // Consider as just a library.
          }
        }
        else if (p.is_a<lib> ()  ||
                 p.is_a<liba> () ||
                 p.is_a<libs> ())
        {
          r.seen_lib = r.seen_lib || true;
        }
        // Some other c-common header/source (say C++ in a C rule) other than
        // a C header (we assume everyone can hanle that).
        //
        else if (p.is_a<cc> () && !(x_header (p, true /* c_hdr */)))
        {
          r.seen_cc = true;
          break;
        }
      }

      return r;
    }

    bool link_rule::
    match (action a, target& t, const string& hint) const
    {
      // NOTE: may be called multiple times and for both inner and outer
      //       operations (see the install rules).

      tracer trace (x, "link_rule::match");

      ltype lt (link_type (t));

      // If this is a group member library, link-up to our group (this is the
      // target group protocol which means this can be done whether we match
      // or not).
      //
      // If we are called for the outer operation (see install rules), then
      // use resolve_group() to delegate to inner.
      //
      if (lt.member_library ())
      {
        if (a.outer ())
          resolve_group (a, t);
        else if (t.group == nullptr)
          t.group = &search (t,
                             lt.utility ? libul::static_type : lib::static_type,
                             t.dir, t.out, t.name);
      }

      match_result r (match (a, t, t.group, lt.type, lt.library ()));

      // If this is some other c-common header/source (say C++ in a C rule),
      // then we shouldn't try to handle that (it may need to be compiled,
      // etc).
      //
      if (r.seen_cc)
      {
        l4 ([&]{trace << "non-" << x_lang << " prerequisite "
                      << "for target " << t;});
        return false;
      }

      if (!(r.seen_x || r.seen_c || r.seen_obj || r.seen_lib))
      {
        l4 ([&]{trace << "no " << x_lang << ", C, or obj/lib prerequisite "
                      << "for target " << t;});
        return false;
      }

      // We will only chain a C source if there is also an X source or we were
      // explicitly told to.
      //
      if (r.seen_c && !r.seen_x && hint < x)
      {
        l4 ([&]{trace << "C prerequisite without " << x_lang << " or hint "
                      << "for target " << t;});
        return false;
      }

      return true;
    }

    auto link_rule::
    derive_libs_paths (file& ls,
                       const char* pfx,
                       const char* sfx) const -> libs_paths
    {
      bool win (tclass == "windows");

      // Get default prefix and extension.
      //
      const char* ext (nullptr);
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
      path lk, ld, so, in;

      // We start with the basic path.
      //
      path b (ls.dir);

      if (pfx != nullptr && pfx[0] != '\0')
      {
        b /= pfx;
        b += ls.name;
      }
      else
        b /= ls.name;

      if (sfx != nullptr && sfx[0] != '\0')
        b += sfx;

      // Clean pattern.
      //
      path cp (b);
      cp += "?*"; // Don't match empty (like the libfoo.so symlink).
      append_ext (cp);

      // On Windows the real path is to libs{} and the link path is empty.
      // Note that we still need to derive the import library path.
      //
      if (win)
      {
        // Usually on Windows with MSVC the import library is called the same
        // as the DLL but with the .lib extension. Which means it clashes with
        // the static library. Instead of decorating the static library name
        // with ugly suffixes (as is customary), let's use the MinGW approach
        // (one must admit it's quite elegant) and call it .dll.lib.
        //
        libi& li (*find_adhoc_member<libi> (ls));

        if (li.path ().empty ())
        {
          path ip (b);
          append_ext (ip);
          li.derive_path (move (ip), tsys == "mingw32" ? "a" : "lib");
        }

        //@@ TMP
        lk = b;
        append_ext (lk);
      }
      else if (!v.empty ())
      {
        lk = b;
        append_ext (lk);
      }

      // See if we need the load name.
      //
      if (const string* s = cast_null<string> (ls["bin.lib.load_suffix"]))
      {
        if (!s->empty ())
        {
          b += *s;
          ld = b;
          append_ext (ld);
        }
      }

      if (!v.empty ())
        b += v;

      const path& re (ls.derive_path (move (b)));

      return libs_paths {
        move (lk), move (ld), move (so), move (in), &re, move (cp)};
    }

    // Look for binary-full utility library recursively until we hit a
    // non-utility "barier".
    //
    static bool
    find_binfull (action a, const target& t, linfo li)
    {
      for (const target* pt: t.prerequisite_targets[a])
      {
        if (pt == nullptr || unmark (pt) != 0) // Called after pass 1 below.
          continue;

        const file* pf;

        // If this is the libu*{} group, then pick the appropriate member.
        //
        if (const libul* ul = pt->is_a<libul> ())
        {
          pf = &link_member (*ul, a, li)->as<file> ();
        }
        else if ((pf = pt->is_a<libue> ()) ||
                 (pf = pt->is_a<libus> ()) ||
                 (pf = pt->is_a<libua> ()))
          ;
        else
          continue;

        if (!pf->path ().empty () || find_binfull (a, *pf, li))
          return true;
      }

      return false;
    };

    recipe link_rule::
    apply (action a, target& xt) const
    {
      tracer trace (x, "link_rule::apply");

      file& t (xt.as<file> ());

      // Note that for_install is signalled by install_rule and therefore
      // can only be relied upon during execute.
      //
      match_data& md (t.data (match_data ()));

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      ltype lt (link_type (t));
      otype ot (lt.type);
      linfo li (link_info (bs, ot));

      // Set the library type (C, C++, etc) as rule-specific variable.
      //
      if (lt.library ())
        t.state[a].assign (c_type) = string (x);

      bool binless (lt.library ()); // Binary-less until proven otherwise.

      // Inject dependency on the output directory. Note that we do it even
      // for binless libraries since there could be other output (e.g., .pc
      // files).
      //
      inject_fsdir (a, t);

      // Process prerequisites, pass 1: search and match prerequisite
      // libraries, search obj/bmi{} targets, and search targets we do rule
      // chaining for.
      //
      // Also clear the binless flag if we see any source or object files.
      // Note that if we don't see any this still doesn't mean the library is
      // binless since it can depend on a binfull utility library. This we
      // check below, after matching the libraries.
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
      // or a subdirectory of our project root. Except for libraries: if we
      // ignore them, then they won't be added to synthesized dependencies and
      // this will break things if we do, say, update after clean in the same
      // invocation. So for libraries we ignore them later, on pass 3.
      //
      optional<dir_paths> usr_lib_dirs; // Extract lazily.
      compile_target_types tts (compile_types (ot));

      auto skip = [&a, &rs] (const target* pt) -> bool
      {
        return a.operation () == clean_id && !pt->dir.sub (rs.out_path ());
      };

      auto& pts (t.prerequisite_targets[a]);
      size_t start (pts.size ());

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        include_type pi (include (a, t, p));

        // We pre-allocate a NULL slot for each (potential; see clean)
        // prerequisite target.
        //
        pts.push_back (prerequisite_target (nullptr, pi));
        const target*& pt (pts.back ());

        if (pi != include_type::normal) // Skip excluded and ad hoc.
          continue;

        // Mark:
        //   0 - lib
        //   1 - src
        //   2 - mod
        //   3 - obj/bmi and also lib not to be cleaned
        //
        uint8_t m (0);

        bool mod (x_mod != nullptr && p.is_a (*x_mod));

        if (mod || p.is_a (x_src) || p.is_a<c> ())
        {
          binless = binless && false;

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
                                  ? (group ? bmi::static_type : tts.bmi)
                                  : (group ? obj::static_type : tts.obj));

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
          {
            pt = nullptr;
            continue;
          }

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
            m = 3; // Mark so it is not matched.

          // If this is the lib{}/libu{} group, then pick the appropriate
          // member.
          //
          if (const libx* l = pt->is_a<libx> ())
            pt = link_member (*l, a, li);
        }
        else
        {
          // If this is the obj{} or bmi{} target group, then pick the
          // appropriate member.
          //
          if      (p.is_a<obj> ()) pt = &search (t, tts.obj, p.key ());
          else if (p.is_a<bmi> ()) pt = &search (t, tts.bmi, p.key ());
          //
          // Windows module definition (.def). For other platforms (and for
          // static libraries) treat it as an ordinary prerequisite.
          //
          else if (p.is_a<def> () && tclass == "windows" && ot != otype::a)
          {
            pt = &p.search (t);
          }
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
              // Note that ad hoc inputs have to be explicitly marked with the
              // include=adhoc prerequisite-specific variable.
              //
              if (current_outer_oif != nullptr)
                continue;
            }

            pt = &p.search (t);
          }

          if (skip (pt))
          {
            pt = nullptr;
            continue;
          }

          // @@ MODHDR: hbmix{} has no objx{}
          //
          binless = binless && !(pt->is_a<objx> () || pt->is_a<bmix> ());

          m = 3;
        }

        mark (pt, m);
      }

      // Match lib{} (the only unmarked) in parallel and wait for completion.
      //
      match_members (a, t, pts, start);

      // Check if we have any binfull utility libraries.
      //
      binless = binless && !find_binfull (a, t, li);

      // Now that we know for sure whether we are binless, derive file name(s)
      // and add ad hoc group members. Note that for binless we still need the
      // .pc member (whose name depends on the libray prefix) so we take care
      // to not derive the path for the library target itself inside.
      //
      {
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

          if (binless)
            t.path (empty_path);
          else
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

              if (binless)
                t.path (empty_path);
              else
                t.derive_path (e, p, s);

              break;
            }
          case otype::s:
            {
              if (binless)
                t.path (empty_path);
              else
              {
                // On Windows libs{} is an ad hoc group. The libs{} itself is
                // the DLL and we add libi{} import library as its member.
                //
                if (tclass == "windows")
                {
                  e = "dll";
                  add_adhoc_member<libi> (t);
                }

                md.libs_paths = derive_libs_paths (t, p, s);
              }

              break;
            }
          }

          // Add VC's .pdb. Note that we are looking for the link.exe /DEBUG
          // option.
          //
          if (!binless && ot != otype::a && tsys == "win32-msvc")
          {
            if (find_option ("/DEBUG", t, c_loptions, true) ||
                find_option ("/DEBUG", t, x_loptions, true))
            {
              const target_type& tt (*bs.find_target_type ("pdb"));

              // We call the target foo.{exe,dll}.pdb rather than just foo.pdb
              // because we can have both foo.exe and foo.dll in the same
              // directory.
              //
              file& pdb (add_adhoc_member<file> (t, tt, e));

              // Note that the path is derived from the exe/dll path (so it
              // will include the version in case of a dll).
              //
              if (pdb.path ().empty ())
                pdb.derive_path (t.path (), "pdb");
            }
          }

          // Add pkg-config's .pc file.
          //
          // Note that we do it regardless of whether we are installing or not
          // for two reasons. Firstly, it is not easy to detect this situation
          // here since the for_install hasn't yet been communicated by
          // install_rule. Secondly, always having this member takes care of
          // cleanup automagically. The actual generation happens in
          // perform_update() below.
          //
          if (ot != otype::e)
          {
            file& pc (add_adhoc_member<file> (t,
                                              (ot == otype::a
                                               ? pca::static_type
                                               : pcs::static_type)));

            // Note that here we always use the lib name prefix, even on
            // Windows with VC. The reason is the user needs a consistent name
            // across platforms by which they can refer to the library. This
            // is also the reason why we use the .static and .shared second-
            // level extensions rather that a./.lib and .so/.dylib/.dll.
            //
            if (pc.path ().empty ())
              pc.derive_path (nullptr, (p == nullptr ? "lib" : p), s);
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
#ifdef _WIN32
            target& dir =
#endif
              add_adhoc_member (t,
                                fsdir::static_type,
                                path_cast<dir_path> (t.path () + ".dlls"),
                                t.out,
                                string () /* name */);

            // By default our backlinking logic will try to symlink the
            // directory and it can even be done on Windows using junctions.
            // The problem is the Windows DLL assembly "logic" refuses to
            // recognize a junction as a valid assembly for some reason. So we
            // are going to resort to copy-link (i.e., a real directory with a
            // bunch of links).
            //
            // Interestingly, the directory symlink works just fine under
            // Wine. So we only resort to copy-link'ing if we are running on
            // Windows.
            //
#ifdef _WIN32
            dir.state[a].assign (var_backlink) = "copy";
#endif
          }
        }
      }

      // Process prerequisites, pass 2: finish rule chaining but don't start
      // matching anything yet since that may trigger recursive matching of
      // bmi{} targets we haven't completed yet. Hairy, I know.
      //

      // Parallel prerequisites/prerequisite_targets loop.
      //
      size_t i (start);
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        const target*& pt (pts[i].target);
        uintptr_t&     pd (pts[i++].data);

        if (pt == nullptr)
          continue;

        // New mark:
        //  1 - completion
        //  2 - verification
        //
        uint8_t m (unmark (pt));

        if (m == 3)                // obj/bmi or lib not to be cleaned
        {
          m = 1; // Just completion.

          // Note that if this is a library not to be cleaned, we keep it
          // marked for completion (see the next phase).
        }
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
            ? &search (t, (mod ? tts.bmi : tts.obj), rt.dir, rt.out, rt.name)
            : &rt;

          const target_type& rtt (mod
                                  ? (group ? bmi::static_type : tts.bmi)
                                  : (group ? obj::static_type : tts.obj));

          // If this obj*{} already has prerequisites, then verify they are
          // "compatible" with what we are doing here. Otherwise, synthesize
          // the dependency. Note that we may also end up synthesizing with
          // someone beating us to it. In this case also verify.
          //
          bool verify (true);

          // Note that we cannot use has_group_prerequisites() since the
          // target is not yet matched. So we check the group directly. Of
          // course, all of this is racy (see below).
          //
          if (!pt->has_prerequisites () &&
              (!group || !rt.has_prerequisites ()))
          {
            prerequisites ps {p.as_prerequisite ()}; // Source.

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

              if (pt == nullptr) // Note: ad hoc is taken care of.
                continue;

              // NOTE: pt may be marked (even for a library -- see clean
              // above). So watch out for a faux pax in this careful dance.
              //
              if (p.is_a<libx> () ||
                  p.is_a<liba> () || p.is_a<libs> () || p.is_a<libux> () ||
                  p.is_a<bmi> ()  || p.is_a (tts.bmi))
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

            // Note: adding to the group, not the member.
            //
            verify = !rt.prerequisites (move (ps));

            // Recheck that the target still has no prerequisites. If that's
            // no longer the case, then verify the result is compatible with
            // what we need.
            //
            // Note that there are scenarios where we will not detect this or
            // the detection will be racy. For example, thread 1 adds the
            // prerequisite to the group and then thread 2, which doesn't use
            // the group, adds the prerequisite to the member. This could be
            // triggered by something like this (undetectable):
            //
            // lib{foo}: cxx{foo}
            // exe{foo}: cxx{foo}
            //
            // Or this (detection is racy):
            //
            // lib{bar}: cxx{foo}
            // liba{baz}: cxx{foo}
            //
            // The current feeling, however, is that in non-contrived cases
            // (i.e., the source file is the same) this should be harmless.
            //
            if (!verify && group)
              verify = pt->has_prerequisites ();
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

      i = start;
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        bool adhoc (pts[i].adhoc);
        const target*& pt (pts[i++]);

        uint8_t m;

        if (pt == nullptr)
        {
          // Handle ad hoc prerequisities.
          //
          if (!adhoc)
            continue;

          pt = &p.search (t);
          m = 1; // Mark for completion.
        }
        else if ((m = unmark (pt)) != 0)
        {
          // If this is a library not to be cleaned, we can finally blank it
          // out.
          //
          if (skip (pt))
          {
            pt = nullptr;
            continue;
          }
        }

        match_async (a, *pt, target::count_busy (), t[a].task_count);
        mark (pt, m);
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
            const target& tp (p.search (t));
            const target& tp1 (p1.search (*pt));

            if (&tp != &tp1)
            {
              bool group (!p.prerequisite.belongs (t));

              const target_type& rtt (mod
                                      ? (group ? bmi::static_type : tts.bmi)
                                      : (group ? obj::static_type : tts.obj));

              fail << "synthesized dependency for prerequisite " << p << " "
                   << "would be incompatible with existing target " << *pt <<
                info << "existing prerequisite " << p1 << " does not match "
                   << p <<
                info << p1 << " resolves to target " << tp1 <<
                info << p << " resolves to target " << tp <<
                info << "specify corresponding " << rtt.name << "{} "
                   << "dependency explicitly";
            }

            break;
          }
        }
      }

      md.binless = binless;

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
      struct data
      {
        strings&             args;
        const file&          l;
        action               a;
        linfo                li;
        compile_target_types tts;
      } d {args, l, a, li, compile_types (li.type)};

      auto imp = [] (const file&, bool la)
      {
        return la;
      };

      auto lib = [&d, this] (const file* const* lc,
                             const string& p,
                             lflags f,
                             bool)
      {
        const file* l (lc != nullptr ? *lc : nullptr);

        if (l == nullptr)
        {
          // Don't try to link a library (whether -lfoo or foo.lib) to a
          // static library.
          //
          if (d.li.type != otype::a)
            d.args.push_back (p);
        }
        else
        {
          bool lu (l->is_a<libux> ());

          // The utility/non-utility case is tricky. Consider these two
          // scenarios:
          //
          // exe -> (libu1-e -> libu1-e) -> (liba) -> libu-a -> (liba1)
          // exe -> (liba) -> libu1-a -> libu1-a -> (liba1) -> libu-a1
          //
          // Libraries that should be linked are in '()'. That is, we need to
          // link the initial sequence of utility libraries and then, after
          // encountering a first non-utility, only link non-utilities
          // (because they already contain their utility's object files).
          //
          if (lu)
          {
            for (ptrdiff_t i (-1); lc[i] != nullptr; --i)
              if (!lc[i]->is_a<libux> ())
                return;
          }

          if (d.li.type == otype::a)
          {
            // Linking a utility library to a static library.
            //
            // Note that utility library prerequisites of utility libraries
            // are automatically handled by process_libraries(). So all we
            // have to do is implement the "thin archive" logic.
            //
            // We may also end up trying to link a non-utility library to a
            // static library via a utility library (direct linking is taken
            // care of by perform_update()). So we cut it off here.
            //
            if (!lu)
              return;

            if (l->mtime () == timestamp_unreal) // Binless.
              return;

            for (const target* pt: l->prerequisite_targets[d.a])
            {
              if (pt == nullptr)
                continue;

              if (modules)
              {
                if (pt->is_a<bmix> ()) // @@ MODHDR: hbmix{} has no objx{}
                  pt = find_adhoc_member (*pt, d.tts.obj);
              }

              // We could have dependency diamonds with utility libraries.
              // Repeats will be handled by the linker (in fact, it could be
              // required to repeat them to satisfy all the symbols) but here
              // we have to suppress duplicates ourselves.
              //
              if (const file* f = pt->is_a<objx> ())
              {
                string p (relative (f->path ()).string ());
                if (find (d.args.begin (), d.args.end (), p) == d.args.end ())
                  d.args.push_back (move (p));
              }
            }
          }
          else
          {
            // Linking a library to a shared library or executable.
            //

            if (l->mtime () == timestamp_unreal) // Binless.
              return;

            // On Windows a shared library is a DLL with the import library as
            // an ad hoc group member. MinGW though can link directly to DLLs
            // (see search_library() for details).
            //
            if (tclass == "windows" && l->is_a<libs> ())
            {
              if (const libi* li = find_adhoc_member<libi> (*l))
                l = li;
            }

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
                d.args.push_back ("-Wl,--whole-archive");
                d.args.push_back (move (p));
                d.args.push_back ("-Wl,--no-whole-archive");
                return;
              }
            }

            d.args.push_back (move (p));
          }
        }
      };

      auto opt = [&d, this] (const file& l,
                             const string& t,
                             bool com,
                             bool exp)
      {
        // Don't try to pass any loptions when linking a static library.
        //
        if (d.li.type == otype::a)
          return;

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

          append_options (d.args, *g, var);
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
      struct data
      {
        sha256&         cs;
        const dir_path& out_root;
        bool&           update;
        timestamp       mt;
        linfo           li;
      } d {cs, bs.root_scope ()->out_path (), update, mt, li};

      auto imp = [] (const file&, bool la)
      {
        return la;
      };

      auto lib = [&d, this] (const file* const* lc,
                             const string& p,
                             lflags f,
                             bool)
      {
        const file* l (lc != nullptr ? *lc : nullptr);

        if (l == nullptr)
        {
          if (d.li.type != otype::a)
            d.cs.append (p);
        }
        else
        {
          bool lu (l->is_a<libux> ());

          if (lu)
          {
            for (ptrdiff_t i (-1); lc[i] != nullptr; --i)
              if (!lc[i]->is_a<libux> ())
                return;
          }

          // We also don't need to do anything special for linking a utility
          // library to a static library. If any of its object files (or the
          // set of its object files) changes, then the library will have to
          // be updated as well. In other words, we use the library timestamp
          // as a proxy for all of its member's timestamps.
          //
          // We do need to cut of the static to static linking, just as in
          // append_libraries().
          //
          if (d.li.type == otype::a && !lu)
            return;

          if (l->mtime () == timestamp_unreal) // Binless.
            return;

          // Check if this library renders us out of date.
          //
          d.update = d.update || l->newer (d.mt);

          // On Windows a shared library is a DLL with the import library as
          // an ad hoc group member. MinGW though can link directly to DLLs
          // (see search_library() for details).
          //
          if (tclass == "windows" && l->is_a<libs> ())
          {
            if (const libi* li = find_adhoc_member<libi> (*l))
              l = li;
          }

          d.cs.append (f);
          hash_path (d.cs, l->path (), d.out_root);
        }
      };

      auto opt = [&d, this] (const file& l,
                             const string& t,
                             bool com,
                             bool exp)
      {
        if (d.li.type == otype::a)
          return;

        if (const target* g = exp && l.is_a<libs> () ? l.group : &l)
        {
          const variable& var (
            com
            ? (exp ? c_export_loptions : c_loptions)
            : (t == x
               ? (exp ? x_export_loptions : x_loptions)
               : var_pool[t + (exp ? ".export.loptions" : ".loptions")]));

          hash_options (d.cs, *g, var);
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
                     bool link) const
    {
      // Use -rpath-link only on targets that support it (Linux, *BSD). Note
      // that we don't really need it for top-level libraries.
      //
      if (link)
      {
        if (tclass != "linux" && tclass != "bsd")
          return;
      }

      auto imp = [link] (const file& l, bool la)
      {
        // If we are not rpath-link'ing, then we only need to rpath interface
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
        return (link ? !la : false) || l.is_a<libux> ();
      };

      // Package the data to keep within the 2-pointer small std::function
      // optimization limit.
      //
      struct
      {
        strings& args;
        bool     link;
      } d {args, link};

      auto lib = [&d, this] (const file* const* lc,
                             const string& f,
                             lflags,
                             bool sys)
      {
        const file* l (lc != nullptr ? *lc : nullptr);

        // We don't rpath system libraries. Why, you may ask? There are many
        // good reasons and I have them written on a napkin somewhere...
        //
        if (sys)
          return;

        if (l != nullptr)
        {
          if (!l->is_a<libs> ())
            return;

          if (l->mtime () == timestamp_unreal) // Binless.
            return;
        }
        else
        {
          // This is an absolute path and we need to decide whether it is
          // a shared or static library. Doesn't seem there is anything
          // better than checking for a platform-specific extension (maybe
          // we should cache it somewhere).
          //
          size_t p (path::traits_type::find_extension (f));

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
        string o (d.link ? "-Wl,-rpath-link," : "-Wl,-rpath,");

        size_t p (path::traits_type::rfind_separator (f));
        assert (p != string::npos);

        o.append (f, 0, (p != 0 ? p : 1)); // Don't include trailing slash.
        d.args.push_back (move (o));
      };

      // In case we don't have the "small function object" optimization.
      //
      const function<bool (const file&, bool)> impf (imp);
      const function<
        void (const file* const*, const string&, lflags, bool)> libf (lib);

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
          if (!link && !la)
          {
            // Top-level shared library dependency.
            //
            if (!f->path ().empty ()) // Not binless.
            {
              // It is either matched or imported so should be a cc library.
              //
              if (!cast_false<bool> (f->vars[c_system]))
                args.push_back (
                  "-Wl,-rpath," + f->path ().directory ().string ());
            }
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

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      match_data& md (t.data<match_data> ());

      // Unless the outer install rule signalled that this is update for
      // install, signal back that we've performed plain update.
      //
      if (!md.for_install)
        md.for_install = false;

      bool for_install (*md.for_install);

      ltype lt (link_type (t));
      otype ot (lt.type);
      linfo li (link_info (bs, ot));
      compile_target_types tts (compile_types (ot));

      bool binless (md.binless);
      assert (ot != otype::e || !binless); // Sanity check.

      // Determine if we are out-of-date.
      //
      bool update (false);
      bool scratch (false);
      timestamp mt (binless ? timestamp_unreal : t.load_mtime ());

      // Update prerequisites. We determine if any relevant non-ad hoc ones
      // render us out-of-date manually below.
      //
      // Note that execute_prerequisites() blanks out all the ad hoc
      // prerequisites so we don't need to worry about them from now on.
      //
      target_state ts;

      if (optional<target_state> s =
          execute_prerequisites (a,
                                 t,
                                 mt,
                                 [] (const target&, size_t) {return false;}))
        ts = *s;
      else
      {
        // An ad hoc prerequisite renders us out-of-date. Let's update from
        // scratch for good measure.
        //
        scratch = update = true;
        ts = target_state::changed;
      }

      // (Re)generate pkg-config's .pc file. While the target itself might be
      // up-to-date from a previous run, there is no guarantee that .pc exists
      // or also up-to-date. So to keep things simple we just regenerate it
      // unconditionally.
      //
      // Also, if you are wondering why don't we just always produce this .pc,
      // install or no install, the reason is unless and until we are updating
      // for install, we have no idea where-to things will be installed.
      //
      if (for_install && lt.library () && !lt.utility)
        pkgconfig_save (a, t, lt.static_library (), binless);

      // If we have no binary to build then we are done.
      //
      if (binless)
      {
        t.mtime (timestamp_unreal);
        return ts;
      }

      // Open the dependency database (do it before messing with Windows
      // manifests to diagnose missing output directory).
      //
      depdb dd (tp + ".d");

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
        if (!for_install && cast_true<bool> (t["bin.rpath.auto"]))
          rpath_timestamp = windows_rpath_timestamp (t, bs, a, li);

        auto p (windows_manifest (t, rpath_timestamp != timestamp_nonexistent));
        path& mf (p.first);
        timestamp mf_mt (p.second);

        if (tsys == "mingw32")
        {
          // Compile the manifest into the object file with windres. While we
          // are going to synthesize an .rc file to pipe to windres' stdin, we
          // will still use .manifest to check if everything is up-to-date.
          //
          manifest = mf + ".o";

          if (mf_mt == timestamp_nonexistent || mf_mt > mtime (manifest))
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

            if (!dry_run)
            {
              auto_rmfile rm (of);

              try
              {
                process pr (rc, args, -1);

                try
                {
                  ofdstream os (move (pr.out_fd));

                  // 1 is resource ID, 24 is RT_MANIFEST. We also need to
                  // escape Windows path backslashes.
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
                  rm.cancel ();
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
            }

            update = true; // Manifest changed, force update.
          }
        }
        else
        {
          manifest = move (mf); // Save for link.exe's /MANIFESTINPUT.

          if (mf_mt == timestamp_nonexistent || mf_mt > mt)
            update = true; // Manifest changed, force update.
        }
      }

      // Check/update the dependency database.
      //
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
      string arg1, arg2;
      strings sargs;

      if (lt.static_library ())
      {
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
        }
        else
        {
          // If the user asked for ranlib, don't try to do its function with
          // -s. Some ar implementations (e.g., the LLVM one) don't support
          // leading '-'.
          //
          arg1 = ranlib ? "rc" : "rcs";

          // For utility libraries use thin archives if possible.
          //
          // Thin archives are supported by GNU ar since binutils 2.19.1 and
          // LLVM ar since LLVM 3.8.0. Note that strictly speaking thin
          // archives also have to be supported by the linker but it is
          // probably safe to assume that the two came from the same version
          // of binutils/LLVM.
          //
          if (lt.utility)
          {
            const string& id (cast<string> (rs["bin.ar.id"]));

            for (bool g (id == "gnu"); g || id == "llvm"; ) // Breakout loop.
            {
              auto mj (cast<uint64_t> (rs["bin.ar.version.major"]));
              if (mj <  (g ? 2 : 3)) break;
              if (mj == (g ? 2 : 3))
              {
                auto mi (cast<uint64_t> (rs["bin.ar.version.minor"]));
                if (mi  < (g ? 18 : 8)) break;
                if (mi == 18 && g)
                {
                  auto pa (cast<uint64_t> (rs["bin.ar.version.patch"]));
                  if (pa < 1) break;
                }
              }

              arg1 += 'T';
              break;
            }
          }

          args.push_back (arg1.c_str ());
        }

        append_options (args, t, c_aoptions);
        append_options (args, t, x_aoptions);
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
          // rpath/rpath-link.
          //
          lookup l;

          if ((l = t["bin.rpath"]) && !l->empty ())
            fail << ctgt << " does not support rpath";

          if ((l = t["bin.rpath_link"]) && !l->empty ())
            fail << ctgt << " does not support rpath-link";
        }
        else
        {
          // Set soname.
          //
          if (lt.shared_library ())
          {
            const libs_paths& paths (md.libs_paths);
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
              arg1 = "-install_name";
              arg2 = "@rpath/" + leaf;
            }
            else
              arg1 = "-Wl,-soname," + leaf;

            if (!arg1.empty ())
              args.push_back (arg1.c_str ());

            if (!arg2.empty ())
              args.push_back (arg2.c_str ());
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
          if (cast_true<bool> (t[for_install
                                 ? "bin.rpath_link.auto"
                                 : "bin.rpath.auto"]))
            rpath_libraries (sargs, t, bs, a, li, for_install /* link */);

          lookup l;

          if ((l = t["bin.rpath"]) && !l->empty ())
            for (const dir_path& p: cast<dir_paths> (l))
              sargs.push_back ("-Wl,-rpath," + p.string ());

          if ((l = t["bin.rpath_link"]) && !l->empty ())
          {
            // Only certain targets support -rpath-link (Linux, *BSD).
            //
            if (tclass != "linux" && tclass != "bsd")
              fail << ctgt << " does not support rpath-link";

            for (const dir_path& p: cast<dir_paths> (l))
              sargs.push_back ("-Wl,-rpath-link," + p.string ());
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

        // @@ Note that we don't hash output options so if one of the ad hoc
        //    members that we manage gets renamed, we will miss a rebuild.

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "options mismatch forcing update of " << t;});
      }

      // Finally, hash and compare the list of input files.
      //
      // Should we capture actual file names or their checksum? The only good
      // reason for capturing actual files is diagnostics: we will be able to
      // pinpoint exactly what is causing the update. On the other hand, the
      // checksum is faster and simpler. And we like simple.
      //
      const file* def (nullptr); // Cached if present.
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
            if (pt->is_a<bmix> ()) // @@ MODHDR: hbmix{} has no objx{}
              pt = find_adhoc_member (*pt, tts.obj);
          }

          const file* f;
          bool la (false), ls (false);

          // We link utility libraries to everything except other utility
          // libraries. In case of linking to liba{} we follow the "thin
          // archive" lead and "see through" to their object file
          // prerequisites (recursively, until we encounter a non-utility).
          //
          if ((f = pt->is_a<objx> ())           ||
              (!lt.utility &&
               (la = (f = pt->is_a<libux> ()))) ||
              (!lt.static_library () &&
               ((la = (f = pt->is_a<liba>  ())) ||
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
          else if ((f = pt->is_a<bin::def> ()))
          {
            if (tclass == "windows" && !lt.static_library ())
            {
              // At least link.exe only allows a single .def file.
              //
              if (def != nullptr)
                fail << "multiple module definition files specified for " << t;

              hash_path (cs, f->path (), rs.out_path ());
              def = f;
            }
            else
              f = nullptr; // Not an input.
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

        // Treat *.libs variable values as inputs, not options.
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
      if (dd.writing () || dd.mtime > mt)
        scratch = update = true;

      dd.close ();

      // If nothing changed, then we are done.
      //
      if (!update)
        return ts;

      // Ok, so we are updating. Finish building the command line.
      //
      string in, out, out1, out2, out3; // Storage.

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

          if (ctype == compiler_type::clang)
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

          if (def != nullptr)
          {
            in = "/DEF:" + relative (def->path ()).string ();
            args.push_back (in.c_str ());
          }

          if (ot == otype::s)
          {
            // On Windows libs{} is the DLL and an ad hoc group member is the
            // import library.
            //
            // This will also create the .exp export file. Its name will be
            // derived from the import library by changing the extension.
            // Lucky for us -- there is no option to name it.
            //
            const file& imp (*find_adhoc_member<libi> (t));

            out2 = "/IMPLIB:";
            out2 += relative (imp.path ()).string ();
            args.push_back (out2.c_str ());
          }

          // If we have /DEBUG then name the .pdb file. It is an ad hoc group
          // member.
          //
          if (find_option ("/DEBUG", args, true))
          {
            const file& pdb (
              *find_adhoc_member<file> (t, *bs.find_target_type ("pdb")));

            out1 = "/PDB:";
            out1 += relative (pdb.path ()).string ();
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
                  // On Windows libs{} is the DLL and an ad hoc group member
                  // is the import library.
                  //
                  const file& imp (*find_adhoc_member<libi> (t));
                  out = "-Wl,--out-implib=" + relative (imp.path ()).string ();
                  args.push_back (out.c_str ());
                }
              }

              args.push_back ("-o");
              args.push_back (relt.string ().c_str ());

              // For MinGW the .def file is just another input.
              //
              if (def != nullptr)
              {
                in = relative (def->path ()).string ();
                args.push_back (in.c_str ());
              }

              break;
            }
          case compiler_class::msvc: assert (false);
          }
        }
      }

      args[0] = ld->recall_string ();

      // Append input files noticing the position of the first.
      //
#ifdef _WIN32
      size_t args_input (args.size ());
#endif

      // The same logic as during hashing above. See also a similar loop
      // inside append_libraries().
      //
      for (const prerequisite_target& p: t.prerequisite_targets[a])
      {
        const target* pt (p.target);

        if (pt == nullptr)
          continue;

        if (modules)
        {
          if (pt->is_a<bmix> ()) // @@ MODHDR: hbmix{} has no objx{}
            pt = find_adhoc_member (*pt, tts.obj);
        }

        const file* f;
        bool la (false), ls (false);

        if ((f = pt->is_a<objx> ())           ||
            (!lt.utility &&
             (la = (f = pt->is_a<libux> ()))) ||
            (!lt.static_library () &&
             ((la = (f = pt->is_a<liba>  ())) ||
              (ls = (f = pt->is_a<libs>  ())))))
        {
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
      // sargs? Because of potential reallocations in sargs.
      //
      for (const string& a: sargs)
        args.push_back (a.c_str ());

      if (!lt.static_library ())
      {
        append_options (args, t, c_libs);
        append_options (args, t, x_libs);
      }

      args.push_back (nullptr);

      // Cleanup old (versioned) libraries. Let's do it even for dry-run to
      // keep things simple.
      //
      if (lt.shared_library ())
      {
        const libs_paths& paths (md.libs_paths);
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

              if (test (*paths.real)   &&
                  test ( paths.interm) &&
                  test ( paths.soname) &&
                  test ( paths.load)   &&
                  test ( paths.link))
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

          // Note: doesn't follow symlinks.
          //
          path_search (p, rm, dir_path () /* start */, path_match_flags::none);
        }
        catch (const system_error&) {} // Ignore errors.
      }
      else if (lt.static_library ())
      {
        // We use relative paths to the object files which means we may end
        // up with different ones depending on CWD and some implementation
        // treat them as different archive members. So remote the file to
        // be sure. Note that we ignore errors leaving it to the archiever
        // to complain.
        //
        if (mt != timestamp_nonexistent)
          try_rmfile (relt, true);
      }

      if (verb == 1)
        text << (lt.static_library () ? "ar " : "ld ") << t;
      else if (verb == 2)
        print_process (args);

      // Do any necessary fixups to the command line to make it runnable.
      //
      // Notice the split in the diagnostics: at verbosity level 1 we print
      // the "logical" command line while at level 2 and above -- what we are
      // actually executing.
      //
      // On Windows we need to deal with the command line length limit. The
      // best workaround seems to be passing (part of) the command line in an
      // "options file" ("response file" in Microsoft's terminology). Both
      // Microsoft's link.exe/lib.exe as well as GNU g??.exe/ar.exe support
      // the same @<file> notation (and with a compatible subset of the
      // content format; see below). Note also that GCC is smart enough to use
      // an options file to call the underlying linker if we called it with
      // @<file>. We will also assume that any other linker that we might be
      // using supports this notation.
      //
      // Note that this is a limitation of the host platform, not the target
      // (and Wine, where these lines are a bit blurred, does not have this
      // length limitation).
      //
#ifdef _WIN32
      auto_rmfile trm;
      string targ;
      {
        // Calculate the would-be command line length similar to how process'
        // implementation does it.
        //
        auto quote = [s = string ()] (const char* a) mutable -> const char*
        {
          return process::quote_argument (a, s);
        };

        size_t n (0);
        for (const char* a: args)
        {
          if (a != nullptr)
          {
            if (n != 0)
              n++; // For the space separator.

            n += strlen (quote (a));
          }
        }

        if (n > 32766) // 32768 - "Unicode terminating null character".
        {
          // Use the .t extension (for "temporary").
          //
          const path& f ((trm = auto_rmfile (relt + ".t")).path);

          try
          {
            ofdstream ofs (f);

            // Both Microsoft and GNU support a space-separated list of
            // potentially-quoted arguments. GNU also supports backslash-
            // escaping (whether Microsoft supports it is unclear; but it
            // definitely doesn't need it for backslashes themselves, for
            // example, in paths).
            //
            bool e (tsys != "win32-msvc"); // Assume GNU if not MSVC.
            string b;

            for (size_t i (args_input), n (args.size () - 1); i != n; ++i)
            {
              const char* a (args[i]);

              if (e) // We will most likely have backslashes so just do it.
              {
                for (b.clear (); *a != '\0'; ++a)
                {
                  if (*a != '\\')
                    b += *a;
                  else
                    b += "\\\\";
                }

                a = b.c_str ();
              }

              ofs << (i != args_input ? " " : "") << quote (a);
            }

            ofs << '\n';
            ofs.close ();
          }
          catch (const io_error& e)
          {
            fail << "unable to write " << f << ": " << e;
          }

          // Replace input arguments with @file.
          //
          targ = '@' + f.string ();
          args.resize (args_input);
          args.push_back (targ.c_str());
          args.push_back (nullptr);

          //@@ TODO: leave .t file if linker failed and verb > 2?
        }
      }
#endif

      if (verb > 2)
        print_process (args);

      // Remove the target file if any of the subsequent (after the linker)
      // actions fail or if the linker fails but does not clean up its mess
      // (like link.exe). If we don't do that, then we will end up with a
      // broken build that is up-to-date.
      //
      auto_rmfile rm;

      if (!dry_run)
      {
        rm = auto_rmfile (relt);

        try
        {
          // VC tools (both lib.exe and link.exe) send diagnostics to stdout.
          // Also, link.exe likes to print various gratuitous messages. So for
          // link.exe we redirect stdout to a pipe, filter that noise out, and
          // send the rest to stderr.
          //
          // For lib.exe (and any other insane linker that may try to pull off
          // something like this) we are going to redirect stdout to stderr.
          // For sane compilers this should be harmless.
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

              // If anything remains in the stream, send it all to stderr.
              // Note that the eof check is important: if the stream is at
              // eof, this and all subsequent writes to the diagnostics stream
              // will fail (and you won't see a thing).
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

          // In a multi-threaded program that fork()'ed but did not exec(), it
          // is unwise to try to do any kind of cleanup (like unwinding the
          // stack and running destructors).
          //
          if (e.child)
          {
            rm.cancel ();
#ifdef _WIN32
            trm.cancel ();
#endif
            exit (1);
          }

          throw failed ();
        }

        // VC link.exe creates an import library and .exp file for an
        // executable if any of its object files export any symbols (think a
        // unit test linking libus{}). And, no, there is no way to suppress
        // it. Well, there is a way: create a .def file with an empty EXPORTS
        // section, pass it to lib.exe to create a dummy .exp (and .lib), and
        // then pass this empty .exp to link.exe. Wanna go this way? Didn't
        // think so. Having no way to disable this, the next simplest thing
        // seems to be just cleaning the mess up.
        //
        // Note also that if at some point we decide to support such "shared
        // executables" (-rdynamic, etc), then it will probably have to be a
        // different target type (exes{}?) since it will need a different set
        // of object files (-fPIC so probably objs{}), etc.
        //
        if (lt.executable () && tsys == "win32-msvc")
        {
          path b (relt.base ());
          try_rmfile (b + ".lib", true /* ignore_errors */);
          try_rmfile (b + ".exp", true /* ignore_errors */);
        }
      }

      if (ranlib)
      {
        const process_path& rl (cast<process_path> (ranlib));

        const char* args[] = {
          rl.recall_string (),
          relt.string ().c_str (),
          nullptr};

        if (verb >= 2)
          print_process (args);

        if (!dry_run)
          run (rl, args);
      }

      // For Windows generate (or clean up) rpath-emulating assembly.
      //
      if (tclass == "windows")
      {
        if (lt.executable ())
          windows_rpath_assembly (t, bs, a, li,
                                  cast<string> (rs[x_target_cpu]),
                                  rpath_timestamp,
                                  scratch);
      }

      if (lt.shared_library ())
      {
        // For shared libraries we may need to create a bunch of symlinks (or
        // fallback to hardlinks/copies on Windows).
        //
        auto ln = [] (const path& f, const path& l)
        {
          if (verb >= 3)
            text << "ln -sf " << f << ' ' << l;

          if (dry_run)
            return;

          try
          {
            try
            {
              // The -f part.
              //
              if (file_exists (l, false /* follow_symlinks */))
                try_rmfile (l);

              mkanylink (f, l, true /* copy */, true /* relative */);
            }
            catch (system_error& e)
            {
              throw pair<entry_type, system_error> (entry_type::symlink,
                                                    move (e));
            }
          }
          catch (const pair<entry_type, system_error>& e)
          {
            const char* w (e.first == entry_type::regular ? "copy"     :
                           e.first == entry_type::symlink ? "symlink"  :
                           e.first == entry_type::other   ? "hardlink" :
                           nullptr);

            fail << "unable to make " << w << ' ' << l << ": " << e.second;
          }
        };

        const libs_paths& paths (md.libs_paths);

        const path& lk (paths.link);
        const path& ld (paths.load);
        const path& so (paths.soname);
        const path& in (paths.interm);

        const path* f (paths.real);

        if (!in.empty ()) {ln (*f, in); f = &in;}
        if (!so.empty ()) {ln (*f, so); f = &so;}
        if (!ld.empty ()) {ln (*f, ld); f = &ld;}
        if (!lk.empty ()) {ln (*f, lk);}
      }
      else if (lt.static_library ())
      {
        // Apple ar (from cctools) for some reason truncates fractional
        // seconds when running on APFS (HFS has a second resolution so it's
        // not an issue there). This can lead to object files being newer than
        // the archive, which is naturally bad news. Filed as bug 49604334,
        // reportedly fixed in Xcode 11 beta 5.
        //
        // Note that this block is not inside #ifdef __APPLE__ because we
        // could be cross-compiling, theoretically. We also make sure we use
        // Apple's ar (which is (un)recognized as 'generic') instead of, say,
        // llvm-ar.
        //
        if (tsys == "darwin" && cast<string> (rs["bin.ar.id"]) == "generic")
        {
          if (!dry_run)
            touch (tp, false /* create */, verb_never);
        }
      }

      if (!dry_run)
      {
        rm.cancel ();
        dd.check_mtime (tp);
      }

      // Should we go to the filesystem and get the new mtime? We know the
      // file has been modified, so instead just use the current clock time.
      // It has the advantage of having the subseconds precision. Plus, in
      // case of dry-run, the file won't be modified.
      //
      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    target_state link_rule::
    perform_clean (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());

      ltype lt (link_type (t));
      const match_data& md (t.data<match_data> ());

      clean_extras extras;
      clean_adhoc_extras adhoc_extras;

      if (md.binless)
        ; // Clean prerequsites/members.
      else
      {
        if (tclass != "windows")
          ; // Everything is the default.
        else if (tsys == "mingw32")
        {
          if (lt.executable ())
          {
            extras = {".d", ".dlls/", ".manifest.o", ".manifest"};
          }

          // For shared and static library it's the default.
        }
        else
        {
          // Assuming MSVC or alike.
          //
          if (lt.executable ())
          {
            // Clean up .ilk in case the user enabled incremental linking
            // (notice that the .ilk extension replaces .exe).
            //
            extras = {".d", ".dlls/", ".manifest", "-.ilk"};
          }
          else if (lt.shared_library ())
          {
            // Clean up .ilk and .exp.
            //
            // Note that .exp is based on the .lib, not .dll name. And with
            // versioning their bases may not be the same.
            //
            extras = {".d", "-.ilk"};
            adhoc_extras.push_back ({libi::static_type, {"-.exp"}});
          }

          // For static library it's the default.
        }

        if (extras.empty ())
          extras = {".d"}; // Default.

#ifdef _WIN32
        extras.push_back (".t"); // Options file.
#endif
        // For shared libraries we may have a bunch of symlinks that we need
        // to remove.
        //
        if (lt.shared_library ())
        {
          const libs_paths& lp (md.libs_paths);

          auto add = [&extras] (const path& p)
          {
            if (!p.empty ())
              extras.push_back (p.string ().c_str ());
          };

          add (lp.link);
          add (lp.load);
          add (lp.soname);
          add (lp.interm);
        }
      }

      return perform_clean_extra (a, t, extras, adhoc_extras);
    }
  }
}
