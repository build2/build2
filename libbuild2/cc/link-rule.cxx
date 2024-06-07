// file      : libbuild2/cc/link-rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/link-rule.hxx>

#include <cstdlib>  // exit()
#include <cstring>  // strlen()

#include <libbutl/filesystem.hxx> // file_exists(), path_search()

#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/rule.hxx>    // lib_rule::build_members()
#include <libbuild2/bin/target.hxx>
#include <libbuild2/bin/utility.hxx>

#include <libbuild2/install/utility.hxx>

#include <libbuild2/cc/target.hxx>  // c, pc*
#include <libbuild2/cc/utility.hxx>

using std::exit;

using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;
    using build2::to_string;

    bool link_rule::
    deduplicate_export_libs (const scope& bs,
                             const vector<name>& ns,
                             names& r,
                             vector<reference_wrapper<const name>>* seen) const
    {
      bool top (seen == nullptr);

      vector<reference_wrapper<const name>> seen_storage;
      if (top)
        seen = &seen_storage;

      // The plan is as follows: resolve the target names in ns into targets
      // and then traverse their interface dependencies recursively removing
      // duplicates from the list r.
      //
      for (auto i (ns.begin ()), e (ns.end ()); i != e; ++i)
      {
        if (i->pair)
        {
          ++i;
          continue;
        }

        const name& n (*i);

        if (n.qualified () ||
            !(n.dir.absolute () && n.dir.normalized ()) ||
            !(n.type == "lib" || n.type == "liba" || n.type != "libs"))
          continue;

        if (!top)
        {
          // Check if we have already seen this library among interface
          // dependencies of our interface dependencies.
          //
          if (find (seen->begin (), seen->end (), n) != seen->end ())
            continue;

          // Remove duplicates. Because we only consider absolute/normalized
          // target names, we can just compare their names.
          //
          for (auto i (r.begin ()); i != r.end (); )
          {
            if (i->pair)
              i += 2;
            else if (*i == n)
              i = r.erase (i);
            else
              ++i;
          }

          // @@ TODO: we could optimize this further by returning false if
          //    there are no viable candidates (e.g., only pairs/qualified/etc
          //    left).
          //
          if (r.empty ())
            return false;
        }

        if (const target* t = search_existing (n, bs))
        {
          // The same logic as in process_libraries().
          //
          const scope& bs (t->base_scope ());

          if (lookup l = t->lookup_original (c_export_libs, false, &bs).first)
          {
            if (!deduplicate_export_libs (bs, cast<vector<name>> (l), r, seen))
              return false;
          }

          if (lookup l = t->lookup_original (x_export_libs, false, &bs).first)
          {
            if (!deduplicate_export_libs (bs, cast<vector<name>> (l), r, seen))
              return false;
          }
        }

        if (!top)
          seen->push_back (n);
      }

      return true;
    }

    optional<path> link_rule::
    find_system_library (const strings& l) const
    {
      assert (!l.empty ());

      // Figure out what we are looking for.
      //
      // See similar code in process_libraries().
      //
      // @@ TODO: should we take the link order into account (but do we do
      //    this when we link system libraries)?
      //
      string n1, n2;
      {
        auto i (l.begin ()), e (l.end ());

        string s (*i);

        if (tsys == "win32-msvc")
        {
          if (s[0] == '/')
          {
            // Some option (e.g., /WHOLEARCHIVE:<name>). Fall through to fail.
          }
          else
          {
            // Presumably a complete name.
            //
            n1 = move (s);
            i++;
          }
        }
        else
        {
          if (s[0] == '-')
          {
            // -l<name>, -l <name> (Note: not -pthread, which is system)
            //
            if (s[1] == 'l')
            {
              if (s.size () == 2) // -l <name>
              {
                if (i + 1 != e)
                  s = *++i;
                else
                  s.clear ();
              }
              else                // -l<name>
                s.erase (0, 2);

              if (!s.empty ())
              {
                i++;

                // Here we need to be consistent with search_library(). Maybe
                // one day we should generalize it to be usable here (though
                // here we don't need library name guessing).
                //
                const char* p ("");
                const char* e1 (nullptr);
                const char* e2 (nullptr);

                if (tclass == "windows")
                {
                  if (tsys == "mingw32")
                  {
                    p = "lib";
                    e1 = ".dll.a";
                    e2 = ".a";
                  }
                  else
                  {
                    e1 = ".dll.lib";
                    e2 = ".lib";
                  }
                }
                else
                {
                  p = "lib";
                  e1 = (tclass == "macos" ? ".dylib" : ".so");
                  e2 = ".a";
                }

                n1 = p + s + e1;
                n2 = e2 != nullptr ? p + s + e2 : string ();
              }
            }
#if 0
            // -framework <name> (Mac OS)
            //
            else if (tsys == "darwin" && l == "-framework")
            {
              // @@ TODO: maybe one day.
            }
#endif
            else
            {
              // Some other option (e.g., -Wl,--whole-archive). Fall through
              // to fail.
            }
          }
          else
          {
            // Presumably a complete name.
            //
            n1 = move (s);
            i++;
          }
        }

        if (i != e)
          fail << "unexpected library name '" << *i << "'";
      }

      path p; // Reuse the buffer.
      for (const dir_path& d: sys_lib_dirs)
      {
        auto exists = [&p, &d] (const string& n)
        {
          return file_exists ((p = d, p /= n),
                              true /* follow_symlinks */,
                              true /* ignore_errors */);
        };

        if (exists (n1) || (!n2.empty () && exists (n2)))
          return p;
      }

      return nullopt;
    }

    link_rule::
    link_rule (data&& d)
        : common (move (d)),
          rule_id (string (x) += ".link 3")
    {
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
        // Note that here we don't validate the update operation override
        // value (since we may not match). Instead we do this in apply().
        //
        lookup l;
        if (include (a, t, p, a.operation () == update_id ? &l : nullptr) !=
              include_type::normal)
          continue;

        if (p.is_a (x_src)                        ||
            (x_mod != nullptr && p.is_a (*x_mod)) ||
            (x_asp != nullptr && p.is_a (*x_asp)) ||
            (x_obj != nullptr && p.is_a (*x_obj)) ||
            // Header-only X library (or library with C source and X header).
            (library          && x_header (p, false /* c_hdr */)))
        {
          r.seen_x = true;
        }
        else if (p.is_a<c> () || p.is_a<S> ()       ||
                 (x_obj != nullptr && p.is_a<m> ()) ||
                 // Header-only C library.
                 (library && p.is_a<h> ()))
        {
          r.seen_c = true;
        }
        else if (p.is_a<obj> () || p.is_a<bmi> ())
        {
          r.seen_obj = true;
        }
        else if (p.is_a<obje> () || p.is_a<bmie> ())
        {
          // We can make these "no-match" if/when there is a valid use case.
          //
          if (ot != otype::e)
            fail << p.type ().name << "{} as prerequisite of " << t;

          r.seen_obj = true;
        }
        else if (p.is_a<obja> () || p.is_a<bmia> ())
        {
          if (ot != otype::a)
            fail << p.type ().name << "{} as prerequisite of " << t;

          r.seen_obj = true;
        }
        else if (p.is_a<objs> () || p.is_a<bmis> ())
        {
          if (ot != otype::s)
            fail << p.type ().name << "{} as prerequisite of " << t;

          r.seen_obj = true;
        }
        else if (p.is_a<libul> () || p.is_a<libux> ())
        {
          // For a unility library we look at its prerequisites, recursively.
          //
          // This is a bit iffy: in our model a rule can only search a
          // target's prerequisites if it matches. But we don't yet know
          // whether we match. However, it seems correct to assume that any
          // rule-specific search will always resolve to an existing target if
          // there is one. So perhaps it's time to relax this restriction a
          // little? Note that this fits particularly well with what we are
          // doing here since if there is no existing target, then there can
          // be no prerequisites.
          //
          // Note, however, that we cannot link-up a prerequisite target
          // member to its group since we are not matching this target. As
          // result we have to do all the steps except for setting t.group and
          // pass both member and group (we also cannot query t.group since
          // it's racy).
          //
          const target* pg (nullptr);
          const target* pt (p.search_existing ());

          auto search = [&t, &p] (const target_type& tt)
          {
            return search_existing (t.ctx, p.prerequisite.key (tt));
          };

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
              // It's possible we have no group but have a member so try that.
              //
              if (ot != otype::e)
              {
                // We know this prerequisite member is a prerequisite since
                // otherwise the above search would have returned the member
                // target.
                //
                pt = search (ot == otype::a
                             ? libua::static_type
                             : libus::static_type);
              }
              else
              {
                // Similar semantics to bin::link_member(): prefer static over
                // shared.
                //
                pt = search (libua::static_type);

                if (pt == nullptr)
                  pt = search (libus::static_type);
              }
            }
          }
          else if (!p.is_a<libue> ())
          {
            // See if we also/instead have a group.
            //
            pg = search (libul::static_type);

            if (pt == nullptr)
              swap (pt, pg);
          }

          if (pt != nullptr)
          {
            // If we are matching a target, use the original output type since
            // that would be the member that we pick.
            //
            otype pot (pt->is_a<libul> () ? ot : link_type (*pt).type);

            // Propagate values according to the "see-through" semantics of
            // utility libraries.
            //
            r |= match (a, *pt, pg, pot, true /* lib */);
          }
          else
            r.seen_lib = true; // Consider as just a library.
        }
        else if (p.is_a<lib> ()  ||
                 p.is_a<liba> () ||
                 p.is_a<libs> ())
        {
          r.seen_lib = true;
        }
        // Some other c-common header/source (say C++ in a C rule) other than
        // a C header (we assume everyone can hanle that) or some other
        // #include'able target.
        //
        else if (p.is_a<cc> ()                     &&
                 !(x_header (p, true /* c_hdr */)) &&
                 !p.is_a (x_inc) && !p.is_a<c_inc> ())
        {
          r.seen_cc = true;
          break;
        }
      }

      return r;
    }

    bool link_rule::
    match (action a, target& t, const string& hint, match_extra&) const
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

      // Sometimes we may need to have a binless library whose only purpose is
      // to export dependencies on other libraries (potentially in a platform-
      // specific manner; think the whole -pthread mess). So allow a library
      // without any sources with a hint.
      //
      if (!(r.seen_x || r.seen_c || r.seen_obj || r.seen_lib || !hint.empty ()))
      {
        l4 ([&]{trace << "no " << x_lang << ", C, obj/lib prerequisite or "
                      << "hint for target " << t;});
        return false;
      }

      // We will only chain a C source if there is also an X source or we were
      // explicitly told to.
      //
      if (r.seen_c && !r.seen_x && hint.empty ())
      {
        l4 ([&]{trace << "C prerequisite without " << x_lang << " or hint "
                      << "for target " << t;});
        return false;
      }

      return true;
    }

    auto link_rule::
    derive_libs_paths (file& t,
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
      const string& e (t.derive_extension (ext));

      auto append_ext = [&e] (path& p)
      {
        if (!e.empty ())
        {
          p += '.';
          p += e;
        }
      };

      // See if we have the load suffix.
      //
      const string& ls (cast_empty<string> (t["bin.lib.load_suffix"]));

      // Figure out the version.
      //
      string ver;
      bool verp (true); // Platform-specific.
      using verion_map = map<optional<string>, string>;
      if (const verion_map* m = cast_null<verion_map> (t["bin.lib.version"]))
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
          i = m->find (string ("*"));

        // Finally look for the platform-independent version.
        //
        if (i == m->end ())
        {
          verp = false;

          i = m->find (nullopt);

          // For backwards-compatibility.
          //
          if (i == m->end ())
            i = m->find (string ());
        }

        // If we didn't find anything, fail. If the bin.lib.version was
        // specified, then it should explicitly handle all the targets.
        //
        if (i == m->end ())
          fail << "no version for " << ctgt << " in bin.lib.version" <<
            info << "considere adding " << tsys << "@<ver> or " << tclass
               << "@<ver>";

        ver = i->second;
      }

      // Now determine the paths.
      //
      path lk, ld, so, in;

      // We start with the basic path.
      //
      path b (t.dir);

      if (pfx != nullptr && pfx[0] != '\0')
      {
        b /= pfx;
        b += t.name;
      }
      else
        b /= t.name;

      if (sfx != nullptr && sfx[0] != '\0')
        b += sfx;

      // Clean patterns.
      //
      // Note that looser patterns tend to match all kinds of unexpected
      // stuff, for example (using Windows; without the lib prefix things are
      // even worse):
      //
      // foo-io.dll
      // foo.dll.obj
      // foo-1.dll.obj
      // foo.dll.u.lib
      //
      // Even with these patterns we tighted things up we do additional
      // filtering (of things like .d, .t that derived from the suffixed
      // and versioned name) at the match site.
      //
      path cp_l, cp_v;

      // Append separator characters (`-`, `_`, maybe `-v`) to the clean
      // pattern until we encounter a digit. Return false if the digit was
      // never encountered.
      //
      auto append_sep = [] (path& cp, const string& s) -> bool
      {
        for (char c: s)
        {
          if (digit (c))
            return true;

          cp += c;
        }

        return false;
      };

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
        libi& i (*find_adhoc_member<libi> (t));

        if (i.path ().empty ())
        {
          path ip (b);
          append_ext (ip);
          i.derive_path (move (ip), tsys == "mingw32" ? "a" : "lib");
        }
      }
      // We will only need the link name if the following name differs.
      //
      else if (!ver.empty () || !ls.empty ())
      {
        lk = b;
        append_ext (lk);
      }

      // See if we have the load suffix.
      //
      if (!ls.empty ())
      {
        // Derive the load suffix clean pattern (e.g., `foo-[0-9]*.dll`).
        //
        // Note: postpone appending the extension since we use this pattern as
        // a base for the version clean pattern.
        //
        cp_l = b;
        if (auto* p = cast_null<string> (t["bin.lib.load_suffix_pattern"]))
          cp_l += *p;
        else if (append_sep (cp_l, ls))
          cp_l += "[0-9]*";
        else
          cp_l.clear (); // Non-digit load suffix (use custom clean pattern).

        b += ls;

        // We will only need the load name if the following name differs.
        //
        if (!ver.empty ())
        {
          ld = b;
          append_ext (ld);
        }
      }

      // Append version and derive the real name.
      //
      const path* re (nullptr);
      if (ver.empty () || !verp)
      {
        if (!ver.empty ())
        {
          // Derive the version clean pattern (e.g., `foo-[0-9]*.dll`, or, if
          // we have the load clean pattern, `foo-[0-9]*-[0-9]*.dll`).
          //
          cp_v = cp_l.empty () ? b : cp_l;

          if (auto* p = cast_null<string> (t["bin.lib.version_pattern"]))
            cp_v += *p;
          else if (append_sep (cp_v, ver))
            cp_v += "[0-9]*";
          else
            cp_v.clear (); // Non-digit version (use custom clean pattern).

          if (!cp_v.empty ())
            append_ext (cp_v);

          b += ver;
        }

        re = &t.derive_path (move (b));
      }
      else
      {
        // Derive the version clean pattern (e.g., `libfoo.so.[0-9]*`, or, if
        // we have the load clean pattern, `libfoo-[0-9]*.so.[0-9]*`).
        //
        cp_v = cp_l.empty () ? b : cp_l;
        append_ext (cp_v);
        cp_v += ".[0-9]*";

        // Parse the next version component in the X.Y.Z version form.
        //
        // Note that we don't bother verifying components are numeric assuming
        // the user knows what they are doing (one can sometimes see versions
        // with non-numeric components though probably not for X).
        //
        auto next = [&ver,
                     b = size_t (0),
                     e = size_t (0)] (const char* what = nullptr) mutable
        {
          if (size_t n = next_word (ver, b, e, '.'))
            return string (ver, b, n);

          if (what != nullptr)
            fail << "missing " << what << " in shared library version '"
                 << ver << "'" << endf;

          return string ();
        };

        if (tclass == "linux")
        {
          // On Linux the shared library version has the MAJOR.MINOR[.EXTRA]
          // form where MAJOR is incremented for backwards-incompatible ABI
          // changes, MINOR -- for backwards-compatible, and optional EXTRA
          // has no specific meaning and can be used as some sort of release
          // or sequence number (e.g., if the ABI has not changed).
          //
          string ma (next ("major component"));
          string mi (next ("minor component"));
          string ex (next ());

          // The SONAME is libfoo.so.MAJOR
          //
          so = b;
          append_ext (so);
          so += '.'; so += ma;

          // If we have EXTRA, then make libfoo.so.MAJOR.MINOR to be the
          // intermediate name.
          //
          if (!ex.empty ())
          {
            in = b;
            append_ext (in);
            in += '.'; in += ma;
            in += '.'; in += mi;
          }

          // Add the whole version as the extra extension(s).
          //
          re = &t.derive_path (move (b),
                               nullptr      /* default_ext */,
                               ver.c_str () /* extra_ext */);
        }
        else
          fail << tclass << "-specific bin.lib.version not yet supported";
      }

      if (!cp_l.empty ()) append_ext (cp_l);

      return libs_paths {
        move (lk),
        move (ld),
        move (so),
        move (in),
        re,
        move (cp_l), move (cp_v)};
    }

    // Look for binful utility library recursively until we hit a non-utility
    // "barier".
    //
    static const libux*
    find_binful (action a, const target& t, linfo li)
    {
      for (const target* pt: t.prerequisite_targets[a])
      {
        if (pt == nullptr || unmark (pt) != 0) // Called after pass 1 below.
          continue;

        const libux* ux;

        // If this is the libu*{} group, then pick the appropriate member.
        //
        if (const libul* ul = pt->is_a<libul> ())
        {
          // @@ Isn't libul{} member already picked or am I missing something?
          //    If not, then we may need the same in recursive-binless logic.
          //
#if 0
          // @@ TMP hm, this hasn't actually been enabled. So may actually
          //    enable and see if it trips up (do git-blame for good measure).
          //
          assert (false); // @@ TMP (remove before 0.16.0 release)
#endif
          ux = &link_member (*ul, a, li)->as<libux> ();
        }
        else if ((ux = pt->is_a<libue> ()) ||
                 (ux = pt->is_a<libus> ()) ||
                 (ux = pt->is_a<libua> ()))
          ;
        else
          continue;

        if (!ux->path ().empty () || (ux = find_binful (a, *ux, li)))
          return ux;
      }

      return nullptr;
    };

    // Given the cc.type value return true if the library is recursively
    // binless.
    //
    static inline bool
    recursively_binless (const string& type)
    {
      size_t p (type.find ("recursively-binless"));
      return (p != string::npos  &&
              type[p - 1] == ',' && // <lang> is first.
              (type[p += 19] == '\0' || type[p] == ','));
    }

    recipe link_rule::
    apply (action a, target& xt, match_extra&) const
    {
      tracer trace (x, "link_rule::apply");

      file& t (xt.as<file> ());
      context& ctx (t.ctx);

      // Note that for_install is signalled by install_rule and therefore
      // can only be relied upon during execute.
      //
      // Note that we don't really need to set it as target data: while there
      // are calls to get it, they should only happen after the target has
      // been matched.
      //
      match_data md (*this);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      ltype lt (link_type (t));
      otype ot (lt.type);
      linfo li (link_info (bs, ot));

      bool binless (lt.library ()); // Binary-less until proven otherwise.
      bool user_binless (lt.library () && cast_false<bool> (t[b_binless]));

      // Inject dependency on the output directory. Note that we do it even
      // for binless libraries since there could be other output (e.g., .pc
      // files).
      //
      const fsdir* dir (inject_fsdir (a, t));

      // Process prerequisites, pass 1: search and match prerequisite
      // libraries, search obj/bmi{} targets, and search targets we do rule
      // chaining for.
      //
      // Also clear the binless flag if we see any source or object files.
      // Note that if we don't see any this still doesn't mean the library is
      // binless since it can depend on a binful utility library. This we
      // check below, after matching the libraries.
      //
      // We do libraries first in order to indicate that we will execute these
      // targets before matching any of the obj/bmi{}. This makes it safe for
      // compile::apply() to unmatch them and therefore not to hinder
      // parallelism (or mess up for-install'ness).
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

      auto skip = [&a, &rs] (const target& pt) -> bool
      {
        return a.operation () == clean_id && !pt.dir.sub (rs.out_path ());
      };

      bool update_match (false); // Have update during match.

      auto& pts (t.prerequisite_targets[a]);
      size_t start (pts.size ());

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // Note that we have to recognize update=match for *(update), not just
        // perform(update). But only actually update for perform(update).
        //
        lookup l; // The `update` variable value, if any.
        include_type pi (
          include (a, t, p, a.operation () == update_id ? &l : nullptr));

        // We pre-allocate a NULL slot for each (potential; see clean)
        // prerequisite target.
        //
        pts.push_back (prerequisite_target (nullptr, pi));
        auto& pto (pts.back ());

        // Use bit 2 of prerequisite_target::include to signal update during
        // match.
        //
        // Not that for now we only allow updating during match ad hoc and
        // mark 3 (headers, etc; see below) prerequisites.
        //
        // By default we update during match headers and ad hoc sources (which
        // are commonly marked as such because they are #include'ed).
        //
        optional<bool> um;

        if (l)
        {
          const string& v (cast<string> (l));

          if (v == "match")
            um = true;
          else if (v == "execute")
            um = false;
          else if (v != "false" && v != "true")
          {
            fail << "unrecognized update variable value '" << v
                 << "' specified for prerequisite " << p.prerequisite;
          }
        }

        // Skip excluded and ad hoc (unless updated during match) on this
        // pass.
        //
        if (pi != include_type::normal)
        {
          if (a == perform_update_id && pi == include_type::adhoc)
          {
            // By default update ad hoc headers/sources during match (see
            // above).
            //
#if 1
            if (!um)
              um = (p.is_a (x_src) || p.is_a<c> () || p.is_a<S> ()          ||
                    (x_mod != nullptr && p.is_a (*x_mod))                   ||
                    (x_obj != nullptr && (p.is_a (*x_obj) || p.is_a<m> ())) ||
                    x_header (p, true));
#endif

            if (*um)
            {
              pto.target = &p.search (t); // mark 0
              pto.include |= prerequisite_target::include_udm;
              update_match = true;
            }
          }

          continue;
        }

        const target*& pt (pto);

        // Mark (2 bits):
        //
        //   0 - lib or update during match
        //   1 - src
        //   2 - mod
        //   3 - obj/bmi and also lib not to be cleaned (and other stuff)
        //
        uint8_t mk (0);

        bool mod (x_mod != nullptr && p.is_a (*x_mod));
        bool hdr (false);

        if (mod                                                   ||
            p.is_a (x_src) || p.is_a<c> () || p.is_a<S> ()        ||
            (x_obj != nullptr && (p.is_a (*x_obj) || p.is_a<m> ())))
        {
          binless = binless && (mod ? user_binless : false);

          // Rule chaining, part 1.
          //
          // Which scope shall we use to resolve the root? Unlikely, but
          // possible, the prerequisite is from a different project
          // altogether. So we are going to use the target's project.

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
          pair<target&, ulock> r (
            search_new_locked (
              ctx, rtt, d, dir_path (), *cp.tk.name, nullptr, cp.scope));

          // If we shouldn't clean obj{}, then it is fair to assume we
          // shouldn't clean the source either (generated source will be in
          // the same directory as obj{} and if not, well, go find yourself
          // another build system ;-)).
          //
          if (skip (r.first))
          {
            pt = nullptr;
            continue;
          }

          // Either set of verify the bin.binless value on this bmi*{} target
          // (see config_data::b_binless for semantics).
          //
          if (mod)
          {
            if (r.second.owns_lock ())
            {
              if (user_binless)
                r.first.assign (b_binless) = true;
            }
            else
            {
              lookup l (r.first[b_binless]);

              if (user_binless ? !cast_false<bool> (l) : l.defined ())
                fail << "synthesized dependency for prerequisite " << p
                     << " would be incompatible with existing target "
                     << r.first <<
                  info << "incompatible bin.binless value";
            }
          }

          pt = &r.first;
          mk = mod ? 2 : 1;
        }
        else if (p.is_a<libx> () ||
                 p.is_a<liba> () ||
                 p.is_a<libs> () ||
                 p.is_a<libux> ())
        {
          // Handle imported libraries.
          //
          if (p.proj ())
            pt = search_library (a, sys_lib_dirs, usr_lib_dirs, p.prerequisite);

          // The rest is the same basic logic as in search_and_match().
          //
          if (pt == nullptr)
            pt = &p.search (t);

          if (skip (*pt))
            mk = 3; // Mark so it is not matched.

          // If this is the lib{}/libul{} group, then pick the appropriate
          // member. Also note this in prerequisite_target::include (used
          // by process_libraries()).
          //
          if (const libx* l = pt->is_a<libx> ())
          {
            pt = link_member (*l, a, li);
            pto.include |= include_group;
          }
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
          else if (p.is_a<def> ())
          {
            if (tclass != "windows" || ot == otype::a)
              continue;

            pt = &p.search (t);
          }
          //
          // Something else. This could be something unrelated that the user
          // tacked on (e.g., a doc{}). Or it could be some ad hoc input to
          // the linker (say a linker script or some such).
          //
          else
          {
            if (!p.is_a<objx> () &&
                !p.is_a<bmix> () &&
                !(hdr = x_header (p, true)))
            {
              // @@ Temporary hack until we get the default outer operation
              // for update. This allows operations like test and install to
              // skip such tacked on stuff. @@ This doesn't feel temporary
              // anymore...
              //
              // Note that ad hoc inputs have to be explicitly marked with the
              // include=adhoc prerequisite-specific variable.
              //
              if (ctx.current_outer_oif != nullptr)
                continue;
            }

            pt = &p.search (t);

            if (pt == dir)
            {
              pt = nullptr;
              continue;
            }
          }

          if (skip (*pt))
          {
            pt = nullptr;
            continue;
          }

          // Header BMIs have no object file. Module BMI must be explicitly
          // marked with bin.binless by the user to be usable in a binless
          // library.
          //
          binless = binless && !(
            pt->is_a<objx> () ||
            (pt->is_a<bmix> ()   &&
             !pt->is_a<hbmix> () &&
             cast_false<bool> ((*pt)[b_binless])));

          mk = 3;
        }

        if (user_binless && !binless)
          fail << t << " cannot be binless due to " << p << " prerequisite";

        // Upgrade update during match prerequisites to mark 0 (see above for
        // details).
        //
        if (a == perform_update_id)
        {
          // By default update headers during match (see above).
          //
#if 1
          if (!um)
            um = hdr;
#endif

          if (*um)
          {
            if (mk != 3)
              fail << "unable to update during match prerequisite " << p <<
                info << "updating this type of prerequisites during match is "
                   << "not supported by this rule";

            mk = 0;
            pto.include |= prerequisite_target::include_udm;
            update_match = true;
          }
        }

        mark (pt, mk);
      }

      // Match lib{} first and then update during match (the only unmarked) in
      // parallel and wait for completion. We need to match libraries first
      // because matching generated headers/sources may lead to matching some
      // of the libraries (for example, if generation requires some of the
      // metadata; think poptions needed by Qt moc).
      //
      {
        auto mask (prerequisite_target::include_udm);

        match_members (a, t, pts, start, {mask, 0});

        if (update_match)
          match_members (a, t, pts, start, {mask, mask});
      }

      // Check if we have any binful utility libraries.
      //
      bool rec_binless (false); // Recursively-binless.
      if (binless)
      {
        if (const libux* l = find_binful (a, t, li))
        {
          binless = false;

          if (user_binless)
            fail << t << " cannot be binless due to binful " << *l
                 << " prerequisite";
        }

        // See if we are recursively-binless.
        //
        if (binless)
        {
          rec_binless = true;

          for (const target* pt: t.prerequisite_targets[a])
          {
            if (pt == nullptr || unmark (pt) != 0) // See above.
              continue;

            const file* ft;
            if ((ft = pt->is_a<libs> ()) ||
                (ft = pt->is_a<liba> ()) ||
                (ft = pt->is_a<libux> ()))
            {
              if (ft->path ().empty ()) // Binless.
              {
                // The same lookup as in process_libraries().
                //
                if (const string* t = cast_null<string> (
                      ft->state[a].lookup_original (
                        c_type, true /* target_only */).first))
                {
                  if (recursively_binless (*t))
                    continue;
                }
              }

              rec_binless = false;
              break;
            }
          }

          // Another thing we must check is for the presence of any simple
          // libraries (-lm, shell32.lib, etc) in *.export.libs. See
          // process_libraries() for details.
          //
          if (rec_binless)
          {
            auto find = [&t, &bs] (const variable& v) -> lookup
            {
              return t.lookup_original (v, false, &bs).first;
            };

            auto has_simple = [] (lookup l)
            {
              if (const auto* ns = cast_null<vector<name>> (l))
              {
                for (auto i (ns->begin ()), e (ns->end ()); i != e; ++i)
                {
                  if (i->pair)
                    ++i;
                  else if (i->simple ()) // -l<name>, etc.
                    return true;
                }
              }

              return false;
            };

            if (lt.shared_library ()) // process_libraries()::impl == false
            {
              if (has_simple (find (x_export_libs)) ||
                  has_simple (find (c_export_libs)))
                rec_binless = false;
            }
            else // process_libraries()::impl == true
            {
              lookup x (find (x_export_impl_libs));
              lookup c (find (c_export_impl_libs));

              if (x.defined () || c.defined ())
              {
                if (has_simple (x) || has_simple (c))
                  rec_binless = false;
              }
              else
              {
                // These are strings and we assume if either is defined and
                // not empty, then we have simple libraries.
                //
                if (((x = find (x_libs)) && !x->empty ()) ||
                    ((c = find (c_libs)) && !c->empty ()))
                  rec_binless = false;
              }
            }
          }
        }
      }

      // Set the library type (C, C++, binless) as rule-specific variable.
      //
      if (lt.library ())
      {
        string v (x);

        if (rec_binless)
          v += ",recursively-binless";
        else if (binless)
          v += ",binless";

        t.state[a].assign (c_type) = move (v);
      }

      // If we have any update during match prerequisites, now is the time to
      // update them. Note that we have to do it before any further matches
      // since they may rely on these prerequisites already being updated (for
      // example, object file matches may need the headers to be already
      // updated). We also must do it after matching all our prerequisite
      // libraries since they may generate headers that we depend upon.
      //
      // Note that we ignore the result and whether it renders us out of date,
      // leaving it to the common execute logic in perform_update().
      //
      // Note also that update_during_match_prerequisites() spoils
      // prerequisite_target::data.
      //
      if (update_match)
        update_during_match_prerequisites (trace, a, t);

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
              else if (tsys == "emscripten")
                e = "js";
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

          // Add Emscripten .wasm.
          //
          if (ot == otype::e && tsys == "emscripten")
          {
            const target_type& tt (*bs.find_target_type ("wasm"));

            file& wasm (add_adhoc_member<file> (t, tt));

            if (wasm.path ().empty ())
              wasm.derive_path ();

            // We don't want to print this member at level 1 diagnostics.
            //
            wasm.state[a].assign (ctx.var_backlink) = names {
              name ("group"), name ("false")};

            // If we have -pthread then we get additional .worker.js file
            // which is used for thread startup. In a somewhat hackish way we
            // represent it as an exe{} member to make sure it gets installed
            // next to the main .js file.
            //
            // @@ Note that our recommendation is to pass -pthread in *.libs
            // but checking that is not straightforward (it could come from
            // one of the libraries that we are linking). We could have called
            // append_libraries() (similar to $x.lib_libs()) and then looked
            // there. But this is quite heavy handed and it's not clear this
            // is worth the trouble since the -pthread support in Emscripten
            // is quite high-touch (i.e., it's not like we can write a library
            // that starts some threads and then run its test as on any other
            // POSIX platform).
            //
            if (find_option ("-pthread", cmode)         ||
                find_option ("-pthread", t, c_loptions) ||
                find_option ("-pthread", t, x_loptions))
            {
              exe& worker (add_adhoc_member<exe> (t, "worker.js"));

              if (worker.path ().empty ())
                worker.derive_path ();

              // We don't want to print this member at level 1 diagnostics.
              //
              worker.state[a].assign (ctx.var_backlink) = names {
                name ("group"), name ("false")};
            }
          }

          // Add VC's .pdb. Note that we are looking for the link.exe /DEBUG
          // option.
          //
          if (!binless && ot != otype::a && tsys == "win32-msvc")
          {
            const string* o;
            if ((o = find_option_prefix ("/DEBUG", t, c_loptions, true)) != nullptr ||
                (o = find_option_prefix ("/DEBUG", t, x_loptions, true)) != nullptr)
            {
              if (icasecmp (*o, "/DEBUG:NONE") != 0)
              {
                const target_type& tt (*bs.find_target_type ("pdb"));

                // We call the target foo.{exe,dll}.pdb rather than just
                // foo.pdb because we can have both foo.exe and foo.dll in the
                // same directory.
                //
                file& pdb (add_adhoc_member<file> (t, tt, e));

                // Note that the path is derived from the exe/dll path (so it
                // will include the version in case of a dll).
                //
                if (pdb.path ().empty ())
                  pdb.derive_path (t.path ());

                // We don't want to print this member at level 1 diagnostics.
                //
                pdb.state[a].assign (ctx.var_backlink) = names {
                  name ("group"), name ("false")};
              }
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
          // Things are even trickier for the common .pc file: we only want to
          // have it in the shared library if we are not installing static
          // (see pkgconfig_save() for details). But we can't know it at this
          // stage. So what we are going to do is conceptually tie the common
          // file to the lib{} group (which does somehow feel correct) by only
          // installing it if the lib{} group is installed. Specifically, here
          // we will use its bin.lib to decide what will be installed and in
          // perform_update() we will confirm that it is actually installed.
          //
          // This, of course, works only if we actually have explicit lib{}.
          // But the user could only have liba{} (common in testing frameworks
          // that provide main()) or only libs{} (e.g., plugin that can also
          // be linked). It's also theoretically possible to have both liba{}
          // and libs{} but no lib{}, in which case it feels correct not to
          // generate the common file at all.
          //
          if (ot != otype::e)
          {
            // Note that here we always use the lib name prefix, even on
            // Windows with VC. The reason is the user needs a consistent name
            // across platforms by which they can refer to the library. This
            // is also the reason why we use the .static and .shared second-
            // level extensions rather that a./.lib and .so/.dylib/.dll.

            // Note also that the order in which we are adding these members
            // is important (see add_addhoc_member() for details).
            //
            if (operator>= (t.group->decl, target_decl::implied) // @@ VC14
                ? ot == (link_members (rs).a ? otype::a : otype::s)
                : search_existing (ctx,
                                   ot == otype::a
                                   ? libs::static_type
                                   : liba::static_type,
                                   t.dir, t.out, t.name) == nullptr)
            {
              auto& pc (add_adhoc_member<pc> (t));

              if (pc.path ().empty ())
                pc.derive_path (nullptr, (p == nullptr ? "lib" : p), s);
            }

            auto& pcx (add_adhoc_member<file> (t,
                                               (ot == otype::a
                                                ? pca::static_type
                                                : pcs::static_type)));

            if (pcx.path ().empty ())
              pcx.derive_path (nullptr, (p == nullptr ? "lib" : p), s);
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
            target& dir (
              add_adhoc_member (t,
                                fsdir::static_type,
                                path_cast<dir_path> (t.path () + ".dlls"),
                                t.out,
                                string () /* name */,
                                nullopt /* ext */));

            // By default our backlinking logic will try to symlink the
            // directory and it can even be done on Windows using junctions.
            // The problem is the Windows DLL assembly "logic" refuses to
            // recognize a junction as a valid assembly for some reason. So we
            // are going to resort to copy-link (i.e., a real directory with a
            // bunch of links). Note also that while DLLs can be symlinked,
            // the assembly manifest cannot (has to be hard-linked or copied).
            //
            // Interestingly, the directory symlink works just fine under
            // Wine. So we only resort to copy-link'ing if we are running on
            // Windows.
            //
            // We also don't want to print this member at level 1 diagnostics.
            //
            dir.state[a].assign (ctx.var_backlink) = names {
#ifdef _WIN32
              name ("copy"), name ("false")
#else
              name ("group"), name ("false")
#endif
            };
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
        //  0 - already matched
        //  1 - completion
        //  2 - verification
        //
        uint8_t mk (unmark (pt));

        if (mk == 3)                 // obj/bmi or lib not to be cleaned
        {
          mk = 1; // Just completion.

          // Note that if this is a library not to be cleaned, we keep it
          // marked for completion (see the next phase).
        }
        else if (mk == 1 || mk == 2) // Source/module chain.
        {
          bool mod (mk == 2); // p is_a x_mod

          mk = 1;

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

          // If this obj*/bmi*{} already has prerequisites, then verify they
          // are "compatible" with what we are doing here. Otherwise,
          // synthesize the dependency. Note that we may also end up
          // synthesizing with someone beating us to it. In this case also
          // verify.
          //
          bool verify (true);

          // Note that we cannot use has_group_prerequisites() since the
          // target is not yet matched. So we check the group directly. Of
          // course, all of this is racy (see below).
          //
          if (!pt->has_prerequisites () &&
              (!group || !rt.has_prerequisites ()))
          {
            prerequisites ps;

            // Add source.
            //
            // Remove the update variable (we may have stray update=execute
            // that was specified together with the header).
            //
            {
              prerequisite pc (p.as_prerequisite ());

              if (!pc.vars.empty ())
                pc.vars.erase (*ctx.var_update);

              ps.push_back (move (pc));
            }

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
            // Note: have similar logic in make_{module,header}_sidebuild().
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
              if (mod
                  ? p1.is_a (*x_mod)
                  : (p1.is_a (x_src) || p1.is_a<c> () || p1.is_a<S> ()       ||
                     (x_obj != nullptr && (p1.is_a (*x_obj) || p1.is_a<m> ()))))
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
                  ((mod                                   ||
                    p.is_a (x_src)                        ||
                    (x_asp != nullptr && p.is_a (*x_asp)) ||
                    (x_obj != nullptr && p.is_a (*x_obj))) && x_header (p1)) ||
                  ((p.is_a<c> () || p.is_a<S> () ||
                    (x_obj != nullptr && p.is_a<m> ())) && p1.is_a<h> ()))
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
                info << "no existing C/" << x_lang << " source prerequisite" <<
                info << "specify corresponding " << rtt.name << "{} "
                   << "dependency explicitly";

            mk = 2; // Needs verification.
          }
        }
        else // lib*{} or update during match
        {
          // If this is a static library, see if we need to link it whole.
          // Note that we have to do it after match since we rely on the
          // group link-up.
          //
          bool u;
          if ((u = pt->is_a<libux> ()) || pt->is_a<liba> ())
          {
            // Note: go straight for the public variable pool.
            //
            const variable& var (ctx.var_pool["bin.whole"]); // @@ Cache.

            // See the bin module for the lookup semantics discussion. Note
            // that the variable is not overridable so we omit find_override()
            // calls.
            //
            lookup l (p.prerequisite.vars[var]);

            if (!l.defined ())
              l = pt->lookup_original (var, true /* target_only */).first;

            if (!l.defined ())
            {
              const target* g (pt->group);

              target_key tk (pt->key ());
              target_key gk (g != nullptr ? g->key () : target_key {});

              l = bs.lookup_original (var,
                                      &tk,
                                      g != nullptr ? &gk : nullptr).first;
            }

            if (l ? cast<bool> (*l) : u)
              pd |= lflag_whole;
          }
        }

        mark (pt, mk);
      }

      // Process prerequisites, pass 3: match everything and verify chains.
      //

      // Wait with unlocked phase to allow phase switching.
      //
      wait_guard wg (ctx, ctx.count_busy (), t[a].task_count, true);

      i = start;
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        bool adhoc (pts[i].adhoc ());
        const target*& pt (pts[i++]);

        uint8_t mk;

        if (pt == nullptr)
        {
          // Handle ad hoc prerequisities.
          //
          if (!adhoc)
            continue;

          pt = &p.search (t);
          mk = 1; // Mark for completion.
        }
        else
        {
          mk = unmark (pt);

          if (mk == 0)
            continue; // Already matched.

          // If this is a library not to be cleaned, we can finally blank it
          // out.
          //
          if (skip (*pt))
          {
            pt = nullptr;
            continue;
          }
        }

        match_async (a, *pt, ctx.count_busy (), t[a].task_count);
        mark (pt, mk);
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
        uint8_t mk;
        if (pt == nullptr || (mk = unmark (pt)) == 0)
          continue;

        match_complete (a, *pt);

        // Nothing else to do if not marked for verification.
        //
        if (mk == 1)
          continue;

        // Finish verifying the existing dependency (which is now matched)
        // compared to what we would have synthesized.
        //
        bool mod (x_mod != nullptr && p.is_a (*x_mod));

        // Note: group already resolved in the previous loop.

        for (prerequisite_member p1: group_prerequisite_members (a, *pt))
        {
          if (mod
              ? p1.is_a (*x_mod)
              : (p1.is_a (x_src) || p1.is_a<c> () || p1.is_a<S> ()        ||
                 (x_obj != nullptr && (p1.is_a (*x_obj) || p1.is_a<m> ()))))
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
      md.start = start;

      switch (a)
      {
        // Keep the recipe (which is match_data) after execution to allow the
        // install rule to examine it.
        //
      case perform_update_id: t.keep_data (a); // Fall through.
      case perform_clean_id: return md;
      default: return noop_recipe; // Configure update.
      }
    }

    // Append (and optionally hash and detect if rendered out of data)
    // libraries to link, recursively.
    //
    void link_rule::
    append_libraries (appended_libraries& ls, strings& args,
                      sha256* cs, bool* update, timestamp mt,
                      const scope& bs, action a,
                      const file& l, bool la, lflags lf, linfo li,
                      optional<bool> for_install, bool self, bool rel,
                      library_cache* lib_cache) const
    {
      struct data
      {
        appended_libraries&  ls;
        strings&             args;

        sha256*              cs;
        const dir_path*      out_root;

        bool*                update;
        timestamp            mt;

        const file&          l;
        action               a;
        linfo                li;
        optional<bool>       for_install;
        bool                 rel;
        compile_target_types tts;
      } d {ls, args,
           cs, cs != nullptr ? &bs.root_scope ()->out_path () : nullptr,
           update, mt,
           l, a, li, for_install, rel, compile_types (li.type)};

      auto imp = [] (const target&, bool la)
      {
        return la;
      };

      auto lib = [&d, this] (
        const target* const* lc,
        const small_vector<reference_wrapper<const string>, 2>& ns,
        lflags f,
        const string* type, // Whole cc.type in the <lang>[,...] form.
        bool)
      {
        // Note: see also make_header_sidebuild().

        const file* l (lc != nullptr ? &(*lc)->as<file> () : nullptr);

        // Suppress duplicates.
        //
        // Linking is the complicated case: we cannot add the libraries and
        // options on the first occurrence of the library and ignore all
        // subsequent occurrences because of the static linking semantics.
        // Instead, we can ignore all the occurrences except the last which
        // would normally be done with a pre-pass but would also complicate
        // things quite a bit. So instead we are going to keep track of the
        // duplicates like we've done in other places but in addition we will
        // also keep track of the elements added to args corresponding to this
        // library. And whenever we see a duplicate, we are going to "hoist"
        // that range of elements to the end of args. See GitHub issue #114
        // for details.
        //
        // One case where we can prune the graph is if the library is
        // recursively-binless. It's tempting to wish that we can do the same
        // just for binless, but alas that's not the case: we have to hoist
        // its binful interface dependency because, for example, it must
        // appear after the preceding static library of which this binless
        // library is a dependency.
        //
        // From the process_libraries() semantics we know that this callback
        // is always called and always after the options callbacks.
        //
        appended_library* al (l != nullptr
                              ? &d.ls.append (*l, d.args.size ())
                              : d.ls.append (ns, d.args.size ()));

        if (al != nullptr && al->end != appended_library::npos) // Closed.
        {
          // Hoist the elements corresponding to this library to the end.
          // Note that we cannot prune the traversal since we need to see the
          // last occurrence of each library, unless the library is
          // recursively-binless (in which case there will be no need to
          // hoist since there can be no libraries among the elements).
          //
          if (type != nullptr && recursively_binless (*type))
            return false;

          d.ls.hoist (d.args, *al);
          return true;
        }

        if (l == nullptr)
        {
          // Don't try to link a library (whether -lfoo or foo.lib) to a
          // static library.
          //
          if (d.li.type != otype::a)
          {
            for (const string& n: ns)
            {
              d.args.push_back (n);

              if (d.cs != nullptr)
                d.cs->append (n);
            }
          }
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
                goto done;
          }
          // If requested, verify the target and the library are both for
          // install or both not. We can only do this if the library is build
          // by our link_rule.
          //
          else if (d.for_install &&
                   type != nullptr &&
                   *type != "cc" &&
                   type->compare (0, 3, "cc,") != 0)
          {
            auto* md (l->try_data<link_rule::match_data> (d.a));

            if (md == nullptr)
              fail << "library " << *l << " is not built with cc module-based "
                   << "link rule" <<
                info << "mark it as generic with cc.type=cc target-specific "
                     << "variable";

            assert (md->for_install); // Must have been executed.

            // The user will get the target name from the context info.
            //
            if (*md->for_install != *d.for_install)
              fail << "incompatible " << *l << " build" <<
                info << "library is built " << (*md->for_install ? "" : "not ")
                     << "for install";
          }

          auto newer = [&d, l] ()
          {
            // @@ Work around the unexecuted member for installed libraries
            // issue (see search_library() for details).
            //
            // Note that the member may not even be matched, let alone
            // executed, so we have to go through the group to detect this
            // case (if the group is not matched, then the member got to be).
            //
#if 0
            return l->newer (d.mt);
#else
            const target* g (l->group);
            target_state s (g != nullptr                           &&
                            g->matched (d.a, memory_order_acquire) &&
                            g->state[d.a].rule == &file_rule::rule_match
                            ? target_state::unchanged
                            : l->executed_state (d.a));

            return l->newer (d.mt, s);
#endif
          };

          if (d.li.type == otype::a)
          {
            // Linking a utility library to a static library.
            //
            // Note that utility library prerequisites of utility libraries
            // are automatically handled by process_libraries(). So all we
            // have to do is implement the "thin archive" logic.
            //
            // We also don't need to do anything special for the out-of-date
            // logic: If any of its object files (or the set of its object
            // files) changes, then the library will have to be updated as
            // well. In other words, we use the library timestamp as a proxy
            // for all of its member's timestamps.
            //
            // We may also end up trying to link a non-utility library to a
            // static library via a utility library (direct linking is taken
            // care of by perform_update()). So we cut it off here.
            //
            if (!lu)
              goto done;

            if (l->mtime () == timestamp_unreal) // Binless.
              goto done;

            // Check if this library renders us out of date.
            //
            if (d.update != nullptr)
              *d.update = *d.update || newer ();

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
                const string& p (d.rel
                                 ? relative (f->path ()).string ()
                                 : f->path ().string ());
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
              goto done;

            // Check if this library renders us out of date.
            //
            if (d.update != nullptr)
              *d.update = *d.update || newer ();

            // On Windows a shared library is a DLL with the import library as
            // an ad hoc group member. MinGW though can link directly to DLLs
            // (see search_library() for details).
            //
            if (tclass == "windows" && l->is_a<libs> ())
            {
              if (const libi* li = find_adhoc_member<libi> (*l))
                l = li;
            }

            string p (d.rel
                      ? relative (l->path ()).string ()
                      : l->path ().string ());

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
                goto done;
              }
            }

            d.args.push_back (move (p));
          }

          if (d.cs != nullptr)
          {
            d.cs->append (f);
            hash_path (*d.cs, l->path (), *d.out_root);
          }
        }

      done:
        if (al != nullptr)
          al->end = d.args.size (); // Close.

        return true;
      };

      auto opt = [&d, this] (const target& lt,
                             const string& t,
                             bool com,
                             bool exp)
      {
        const file& l (lt.as<file> ());

        // Don't try to pass any loptions when linking a static library.
        //
        // Note also that we used to pass non-export loptions but that didn't
        // turn out to be very natural. Specifically, we would end up linking
        // things like version scripts (used to build the shared library
        // variant) when linking the static variant. So now any loptions must
        // be explicitly exported. Note that things are a bit fuzzy when it
        // comes to utility libraries so let's keep the original logic with
        // the exp checks below.
        //
        if (d.li.type == otype::a || !exp)
          return true;

        // Suppress duplicates.
        //
        if (d.ls.append (l, d.args.size ()).end != appended_library::npos)
          return true;

        // If we need an interface value, then use the group (lib{}).
        //
        if (const target* g = exp && l.is_a<libs> () ? l.group : &l)
        {
          // Note: go straight for the public variable pool.
          //
          const variable& var (
            com
            ? (exp ? c_export_loptions : c_loptions)
            : (t == x
               ? (exp ? x_export_loptions : x_loptions)
               : l.ctx.var_pool[t + (exp ? ".export.loptions" : ".loptions")]));

          append_options (d.args, *g, var);

          if (d.cs != nullptr)
            append_options (*d.cs, *g, var);
        }

        return true;
      };

      process_libraries (a, bs, li, sys_lib_dirs,
                         l, la,
                         lf, imp, lib, opt,
                         self,
                         false /* proc_opt_group */,
                         lib_cache);
    }

    void link_rule::
    rpath_libraries (rpathed_libraries& ls, strings& args,
                     const scope& bs,
                     action a, const file& l, bool la,
                     linfo li, bool link, bool self,
                     library_cache* lib_cache) const
    {
      // Use -rpath-link only on targets that support it (Linux, *BSD). Note
      // that we don't really need it for top-level libraries.
      //
      // Note that more recent versions of FreeBSD are using LLVM lld without
      // any mentioning of -rpath-link in the man pages.
      //
      auto have_link = [this] () {return tclass == "linux" || tclass == "bsd";};

      if (link)
      {
        if (!have_link ())
          return;
      }

      auto imp = [link] (const target& l, bool la)
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
        rpathed_libraries& ls;
        strings&           args;
        bool               rpath;
        bool               rpath_link;
      } d {ls, args, false, false};

      if (link)
        d.rpath_link = true;
      else
      {
        // While one would naturally expect -rpath to be a superset of
        // -rpath-link, according to GNU ld:
        //
        // "The -rpath option is also used when locating shared objects which
        //  are needed by shared objects explicitly included in the link; see
        //  the description of the -rpath-link option. Searching -rpath in
        //  this way is only supported by native linkers and cross linkers
        //  which have been configured with the --with-sysroot option."
        //
        // So we check if this is cross-compilation and request both options
        // if that's the case (we have no easy way of detecting whether the
        // linker has been configured with the --with-sysroot option, whatever
        // that means, so we will just assume the worst case).
        //
        d.rpath = true;

        if (have_link ())
        {
          // Detecting cross-compilation is not as easy as it seems. Comparing
          // complete target triplets proved too strict. For example, we may be
          // running on x86_64-apple-darwin17.7.0 while the compiler is
          // targeting x86_64-apple-darwin17.3.0. Also, there is the whole i?86
          // family of CPUs which, at least for linking, should probably be
          // considered the same.
          //
          const target_triplet& h (*bs.ctx.build_host);
          const target_triplet& t (ctgt);

          auto x86 = [] (const string& c)
          {
            return (c.size () == 4 &&
                    c[0] == 'i' &&
                    (c[1] >= '3' && c[1] <= '6') &&
                    c[2] == '8' &&
                    c[3] == '6');
          };

          if (t.system != h.system ||
              (t.cpu != h.cpu && !(x86 (t.cpu) && x86 (h.cpu))))
            d.rpath_link = true;
        }
      }

      auto lib = [&d, this] (
        const target* const* lc,
        const small_vector<reference_wrapper<const string>, 2>& ns,
        lflags,
        const string*,
        bool sys)
      {
        const file* l (lc != nullptr ? &(*lc)->as<file> () : nullptr);

        // We don't rpath system libraries. Why, you may ask? There are many
        // good reasons and I have them written on a napkin somewhere...
        //
        // Well, the main reason is that we naturally assume the dynamic
        // linker searches there by default and so there is no need for rpath.
        // Plus, rpath would prevent "overriding" distribution-system
        // (/usr/lib) libraries with user-system (/usr/local/lib).
        //
        // Note, however, that some operating systems don't search in
        // /usr/local/lib by default (for example, Fedora, RHEL, Mac OS since
        // version 13). In a sense, on these platforms /usr/local is
        // "half-system" in that the system compiler by default searches in
        // /usr/local/include and/or /usr/local/lib (see config_module::init()
        // for background) but the dynamic linker does not. While we could
        // hack this test for such platforms and add rpath for /usr/local/lib,
        // this is still feels wrong (the user can always "fix" such an
        // operating system by instructing the dynamic linker to search in
        // /usr/local/lib, as many, including ourselves, do). So for now we
        // are not going to do anything. In the end, the user can always add
        // an rpath for /usr/local/lib manually.
        //
        // We also assume system libraries can only depend on other system
        // libraries and so can prune the traversal.
        //
        if (sys)
          return false;

        auto append = [&d] (const string& f)
        {
          size_t p (path::traits_type::rfind_separator (f));
          assert (p != string::npos);

          // For good measure, also suppress duplicates at the options level.
          // This will take care of different libraries built in the same
          // directory, system-installed, etc.

          if (d.rpath)
          {
            string o ("-Wl,-rpath,");
            o.append (f, 0, (p != 0 ? p : 1)); // Don't include trailing slash.

            if (find (d.args.begin (), d.args.end (), o) == d.args.end ())
              d.args.push_back (move (o));
          }

          if (d.rpath_link)
          {
            string o ("-Wl,-rpath-link,");
            o.append (f, 0, (p != 0 ? p : 1));

            if (find (d.args.begin (), d.args.end (), o) == d.args.end ())
              d.args.push_back (move (o));
          }
        };

        if (l != nullptr)
        {
          // Suppress duplicates.
          //
          // We handle rpath similar to the compilation case by adding the
          // options on the first occurrence and ignoring (and pruning) all
          // the subsequent.
          //
          if (find (d.ls.begin (), d.ls.end (), l) != d.ls.end ())
            return false;

          // Note that these checks are fairly expensive so we do them after
          // duplicate suppression.
          //
          if (!l->is_a<libs> ())
            return true;

          if (l->mtime () == timestamp_unreal) // Binless.
            return true;

          append (ns[0]);
          d.ls.push_back (l);
        }
        else
        {
          // This is an absolute path and we need to decide whether it is
          // a shared or static library. Doesn't seem there is anything
          // better than checking for a platform-specific extension (maybe
          // we should cache it somewhere).
          //
          for (const string& f: ns)
          {
            size_t p (path::traits_type::find_extension (f));

            if (p == string::npos)
              break;

            ++p; // Skip dot.

            bool c (true);
            const char* e;

            if      (tclass == "windows") {e = "dll"; c = false;}
            else if (tsys == "darwin")     e = "dylib";
            else                           e = "so";

            if ((c
                 ? f.compare (p, string::npos, e)
                 : icasecmp (f.c_str () + p, e)) == 0)
            {
              append (f);
            }
          }
        }

        return true;
      };

      if (self && !link && !la)
      {
        // Top-level shared library dependency.
        //
        // As above, suppress duplicates.
        //
        if (find (d.ls.begin (), d.ls.end (), &l) != d.ls.end ())
          return;

        if (!l.path ().empty ()) // Not binless.
        {
          // It is either matched or imported so should be a cc library.
          //
          if (!cast_false<bool> (l.vars[c_system]))
          {
            string o ("-Wl,-rpath," + l.path ().directory ().string ());

            if (find (args.begin (), args.end (), o) == args.end ())
              args.push_back (move (o));

            ls.push_back (&l);
          }
        }
      }

      process_libraries (a, bs, li, sys_lib_dirs,
                         l, la, 0 /* lflags */,
                         imp, lib, nullptr,
                         false /* self */,
                         false /* proc_opt_group */,
                         lib_cache);
    }

    void link_rule::
    rpath_libraries (strings& args,
                     const scope& bs, action a,
                     const target& t, linfo li, bool link) const
    {
      rpathed_libraries ls;
      library_cache lc;

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
          rpath_libraries (ls, args, bs, a, *f, la, li, link, true, &lc);
        }
      }
    }

    // Append (and optionally hash while at it) object files of bmi{}
    // prerequisites that belong to binless libraries.
    //
    void link_rule::
    append_binless_modules (strings& args, sha256* cs,
                            const scope& bs, action a, const file& t) const
    {
      // Note that here we don't need to hoist anything on duplicate detection
      // since the order in which we link object files is not important.
      //
      for (const target* pt: t.prerequisite_targets[a])
      {
        if (pt != nullptr     &&
            pt->is_a<bmix> () &&
            cast_false<bool> ((*pt)[b_binless]))
        {
          const objx& o (*find_adhoc_member<objx> (*pt)); // Must be there.
          string p (relative (o.path ()).string ());
          if (find (args.begin (), args.end (), p) == args.end ())
          {
            args.push_back (move (p));

            if (cs != nullptr)
              hash_path (*cs, o.path (), bs.root_scope ()->out_path ());

            append_binless_modules (args, cs, bs, a, o);
          }
        }
      }
    }

    // Filter link.exe noise (msvc.cxx).
    //
    void
    msvc_filter_link (diag_buffer&, const file&, otype);

    // Translate target CPU to the link.exe/lib.exe /MACHINE option.
    //
    const char*
    msvc_machine (const string& cpu); // msvc.cxx

    target_state link_rule::
    perform_update (action a, const target& xt, match_data& md) const
    {
      tracer trace (x, "link_rule::perform_update");

      const file& t (xt.as<file> ());
      const path& tp (t.path ());

      context& ctx (t.ctx);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

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
      assert (!lt.executable() || !binless); // Sanity check.

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
      // There is an interesting trade-off between the straight and reverse
      // execution. With straight we may end up with inaccurate progress if
      // most of our library prerequisites (typically specified last) are
      // already up to date. In this case, the progress will first increase
      // slowly as we compile this target's source files and then jump
      // straight to 100% as we "realize" that all the libraries (and all
      // their prerequisites) are already up to date.
      //
      // Switching to reverse fixes this but messes up incremental building:
      // now instead of starting to compile source files right away, we will
      // first spend some time making sure all the libraries are up to date
      // (which, in case of an error in the source code, will be a complete
      // waste).
      //
      // There doesn't seem to be an easy way to distinguish between
      // incremental and from-scratch builds and on balance fast incremental
      // builds feel more important.
      //
      target_state ts;

      if (optional<target_state> s = execute_prerequisites (
            a, t,
            mt,
            [] (const target&, size_t) {return false;}))
      {
        ts = *s;
      }
      else
      {
        // An ad hoc prerequisite renders us out-of-date. Let's update from
        // scratch for good measure.
        //
        scratch = update = true;
        ts = target_state::changed;
      }

      // Check for the for_install variable on each prerequisite and blank out
      // those that don't match. Note that we have to do it after updating
      // prerequisites to keep the dependency counts straight.
      //
      if (const variable* var_fi = rs.var_pool ().find ("for_install"))
      {
        // Parallel prerequisites/prerequisite_targets loop.
        //
        size_t i (md.start);
        for (prerequisite_member p: group_prerequisite_members (a, t))
        {
          const target*& pt (t.prerequisite_targets[a][i++]);

          if (pt == nullptr)
            continue;

          if (lookup l = p.prerequisite.vars[var_fi])
          {
            if (cast<bool> (l) != for_install)
            {
              l5 ([&]{trace << "excluding " << *pt << " due to for_install";});
              pt = nullptr;
            }
          }
        }
      }

      // (Re)generate pkg-config's .pc file. While the target itself might be
      // up-to-date from a previous run, there is no guarantee that .pc exists
      // or also up-to-date. So to keep things simple we just regenerate it
      // unconditionally (and avoid doing so on uninstall; see pkgconfig_save()
      // for details).
      //
      // Also, if you are wondering why don't we just always produce this .pc,
      // install or no install, the reason is unless and until we are updating
      // for install, we have no idea where-to things will be installed.
      //
      // There is a further complication: we may have no intention of
      // installing the library but still need to update it for install (see
      // install_scope() for background). In which case we may still not have
      // the installation directories. We handle this in pkgconfig_save() by
      // skipping the generation of .pc files (and letting the install rule
      // complain if we do end up trying to install them).
      //
      if (for_install && lt.library () && !lt.utility)
      {
        bool la (lt.static_library ());

        pkgconfig_save (a, t, la, false /* common */, binless);

        // Generate the common .pc file if the lib{} rule is matched (see
        // apply() for details on this two-stage logic).
        //
        auto* m (find_adhoc_member<pc> (t)); // Will be pca/pcs if not found.

        if (!m->is_a (la ? pca::static_type : pcs::static_type))
        {
          if (operator>= (t.group->decl, target_decl::implied) // @@ VC14
              ? t.group->matched (a)
              : true)
          {
            pkgconfig_save (a, t, la, true /* common */, binless);
          }
          else
            // Mark as non-existent not to confuse the install rule.
            //
            m->mtime (timestamp_nonexistent);
        }
      }

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

      // Adjust the environment.
      //
      using environment = small_vector<string, 1>;
      environment env;
      sha256 env_cs;

      // If we have the search paths in the binutils pattern, prepend them to
      // the PATH environment variable so that any dependent tools (such as
      // mt.exe that is invoked by link.exe) are first search for in there.
      //
      {
        bin::pattern_paths pat (bin::lookup_pattern (rs));

        if (pat.paths != nullptr)
        {
          string v ("PATH=");
          v += pat.paths;

          env_cs.append (v); // Hash only what we are adding.

          if (optional<string> o = getenv ("PATH"))
          {
            v += path::traits_type::path_separator;
            v += *o;
          }

          env.push_back (move (v));
        }
      }

      // Convert the environment to what's expected by the process API.
      //
      small_vector<const char*, environment::small_size + 1> env_ptrs;
      if (!env.empty ())
      {
        for (const string& v: env)
          env_ptrs.push_back (v.c_str ());

        env_ptrs.push_back (nullptr);
      }

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

            if (!ctx.dry_run)
            {
              auto_rmfile rm (of);

              try
              {
                // We assume that what we write to stdin is small enough to
                // fit into the pipe's buffer without blocking.
                //
                process pr (rc,
                            args,
                            -1                      /* stdin  */,
                            1                       /* stdout */,
                            diag_buffer::pipe (ctx) /* stderr */,
                            nullptr                 /* cwd    */,
                            env_ptrs.empty () ? nullptr : env_ptrs.data ());

                diag_buffer dbuf (ctx, args[0], pr);

                try
                {
                  ofdstream os (move (pr.out_fd));

                  // 1 is resource ID, 24 is RT_MANIFEST. We also need to
                  // escape Windows path backslashes.
                  //
                  os << "1 24 \"" << sanitize_strlit (mf.string ()) << '"'
                     << endl;

                  os.close ();
                  rm.cancel ();
                }
                catch (const io_error& e)
                {
                  if (run_wait (args, pr))
                    fail << "unable to pipe resource file to " << args[0]
                         << ": " << e;

                  // If the child process has failed then assume the io error
                  // was caused by that and let run_finish() deal with it.
                }

                dbuf.read ();
                run_finish (dbuf, args, pr, 2 /* verbosity */);
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
        // Note: go straight for the public variable pool.
        //
        const string& cs (
          cast<string> (
            rs[tsys == "win32-msvc"
               ? ctx.var_pool["bin.ld.checksum"]
               : x_checksum]));

        if (dd.expect (cs) != nullptr)
          l4 ([&]{trace << "linker mismatch forcing update of " << t;});
      }

      // Then the linker environment checksum (original and our modifications).
      //
      {
        bool e (dd.expect (env_checksum) != nullptr);
        if (dd.expect (env_cs.string ()) != nullptr || e)
          l4 ([&]{trace << "environment mismatch forcing update of " << t;});
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
      // second is simpler. Let's got with the simpler for now (also see a
      // note on the cost of library dependency graph traversal below).
      //
      cstrings args {nullptr}; // Reserve one for config.bin.ar/config.x.
      strings sargs;           // Argument tail with storage.

      // Stored args.
      //
      string arg1, arg2;
      strings sargs1;

      // Shallow-copy over stored args to args. Note that this must only be
      // done once we are finished appending to stored args because of
      // potential reallocations.
      //
      auto append_args = [&args] (const strings& sargs)
      {
        for (const string& a: sargs)
          args.push_back (a.c_str ());
      };

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

          // For utility libraries use thin archives if possible.
          //
          // LLVM's lib replacement had the /LLVMLIBTHIN option at least from
          // version 3.8 so we will assume always.
          //
          if (lt.utility)
          {
            const string& id (cast<string> (rs["bin.ar.id"]));

            if (id == "msvc-llvm")
              args.push_back ("/LLVMLIBTHIN");
          }
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
          // @@ Note also that GNU ar deprecated -T in favor of --thin in
          // version 2.38.
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
        // Are we using the compiler or the linker (e.g., link.exe) directly?
        //
        bool ldc (tsys != "win32-msvc");

        if (ldc)
        {
          append_options (args, t, c_coptions);
          append_options (args, t, x_coptions);
        }

        // Note that these come in the reverse order of coptions since the
        // library search paths are examined in the order specified (in
        // contrast to the "last value wins" semantics that we assume for
        // coptions).
        //
        append_options (args, t, x_loptions);
        append_options (args, t, c_loptions);

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
            rpath_libraries (sargs, bs, a, t, li, for_install /* link */);

          lookup l;
          if ((l = t["bin.rpath"]) && !l->empty ())
          {
            // See if we need to make the specified paths relative using the
            // $ORIGIN (Linux, BSD) or @loader_path (Mac OS) mechanisms.
            //
            optional<dir_path> origin;
            if (for_install && cast_false<bool> (rs["install.relocatable"]))
            {
              // Note that both $ORIGIN and @loader_path will be expanded to
              // the path of the binary that we are building (executable or
              // shared library) as opposed to top-level executable.
              //
              path p (install::resolve_file (t));

              // If the file is not installable then the install.relocatable
              // semantics does not apply, naturally.
              //
              if (!p.empty ())
                origin = p.directory ();
            }

            // Note: suppress duplicates at the options level, similar to
            // rpath_libraries().

            bool origin_used (false);
            for (const dir_path& p: cast<dir_paths> (l))
            {
              string o ("-Wl,-rpath,");

              // Note that we only rewrite absolute paths so if the user
              // specified $ORIGIN or @loader_path manually, we will pass it
              // through as is.
              //
              if (origin && p.absolute ())
              {
                dir_path l;
                try
                {
                  l = p.relative (*origin);
                }
                catch (const invalid_path&)
                {
                  fail << "unable to make rpath " << p << " relative to "
                       << *origin <<
                    info << "required for relocatable installation";
                }

                o += (tclass == "macos" ? "@loader_path" : "$ORIGIN");

                if (!l.empty ())
                {
                  o += path_traits::directory_separator;
                  o += l.string ();
                }

                origin_used = true;
              }
              else
                o += p.string ();

              if (find (sargs.begin (), sargs.end (), o) == sargs.end ())
                sargs.push_back (move (o));
            }

            // According to the Internet, `-Wl,-z,origin` is not needed except
            // potentially for older BSDs.
            //
            if (origin_used && tclass == "bsd")
              sargs.push_back ("-Wl,-z,origin");
          }

          if ((l = t["bin.rpath_link"]) && !l->empty ())
          {
            // Only certain targets support -rpath-link (Linux, *BSD).
            //
            if (tclass != "linux" && tclass != "bsd")
              fail << ctgt << " does not support rpath-link";

            for (const dir_path& p: cast<dir_paths> (l))
            {
              string o ("-Wl,-rpath-link," + p.string ());

              if (find (sargs.begin (), sargs.end (), o) == sargs.end ())
                sargs.push_back (move (o));
            }
          }
        }

        if (ldc)
        {
          // See the compile rule for details. Note that here we don't really
          // know whether it's a C++ executable so we may end up with some
          // unnecessary overhead.
          //
          if (ctype == compiler_type::clang && cvariant == "emscripten")
          {
            if (!find_option_prefix ("DISABLE_EXCEPTION_CATCHING=", args))
            {
              args.push_back ("-s");
              args.push_back ("DISABLE_EXCEPTION_CATCHING=0");
            }
          }

          append_options (args, cmode);
        }

        // Extra system library dirs (last).
        //
        assert (sys_lib_dirs_mode + sys_lib_dirs_extra <= sys_lib_dirs.size ());

        // Note that the mode options are added as part of cmode.
        //
        auto b (sys_lib_dirs.begin () + sys_lib_dirs_mode);
        auto x (b + sys_lib_dirs_extra);

        if (tsys == "win32-msvc")
        {
          // If we have no LIB environment variable set, then we add all of
          // them. But we want extras to come first.
          //
          for (auto i (b); i != x; ++i)
            sargs1.push_back ("/LIBPATH:" + i->string ());

          if (!getenv ("LIB"))
          {
            for (auto i (x), e (sys_lib_dirs.end ()); i != e; ++i)
              sargs1.push_back ("/LIBPATH:" + i->string ());
          }

          append_args (sargs1);
        }
        else if (b != x)
        {
          // Use the more canonical combined form (-L/usr/local/lib) even
          // though it's less efficient (the split one is just too much of an
          // eye-sore in the logs).
          //
          append_combined_option_values (
            sargs1,
            "-L",
            b, x,
            [] (const dir_path& d) -> const string& {return d.string ();});

          append_args (sargs1);
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
      // Note that originally we only hashed inputs here and then re-collected
      // them below. But the double traversal of the library graph proved to
      // be way more expensive on libraries with lots of dependencies (like
      // Boost) than both collecting and hashing in a single pass. So that's
      // what we do now. @@ TODO: it would be beneficial to also merge the
      // rpath pass above into this.
      //
      // See also a similar loop inside append_libraries().
      //
      bool seen_obj (false);
      const file* def (nullptr); // Cached if present.
      {
        appended_libraries als;
        library_cache lc;
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
            {
              pt = find_adhoc_member (*pt, tts.obj);

              if (pt == nullptr) // Header BMIs have no object file.
                continue;
            }
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
            // Link all the dependent interface libraries (shared) or
            // interface and implementation (static), recursively.
            //
            // Also check if any of them render us out of date. The tricky
            // case is, say, a utility library (static) that depends on a
            // shared library. When the shared library is updated, there is no
            // reason to re-archive the utility but those who link the utility
            // have to "see through" the changes in the shared library.
            //
            if (la || ls)
            {
              append_libraries (als, sargs,
                                &cs, &update, mt,
                                bs, a, *f, la, p.data, li,
                                for_install, true, true, &lc);
              f = nullptr; // Timestamp checked by append_libraries().
            }
            else
            {
              // Do not hoist libraries over object files since such object
              // files might satisfy symbols in the preceding libraries.
              //
              als.clear ();

              const path& p (f->path ());
              sargs.push_back (relative (p).string ());
              hash_path (cs, p, rs.out_path ());

              // @@ Do we actually need to hash this? I don't believe this set
              // can change without rendering the object file itself out of
              // date. Maybe in some pathological case where the bmi*{} is
              // marked with bin.binless manually?
              //
              if (modules)
                append_binless_modules (sargs, &cs, bs, a, *f);

              seen_obj = true;
            }
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
          append_options (cs, t, c_libs);
          append_options (cs, t, x_libs);
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

      path reli; // Import library.
      if (lt.shared_library () && (tsys == "win32-msvc" || tsys == "mingw32"))
        reli = relative (find_adhoc_member<libi> (t)->path ());

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
            // See the runtime selection code in the compile rule for details
            // on what's going on here.
            //
            initializer_list<const char*> os {"-nostdlib", "-nostartfiles"};
            if (!find_options (os, cmode)         &&
                !find_options (os, t, c_coptions) &&
                !find_options (os, t, x_coptions))
            {
              args.push_back ("/DEFAULTLIB:msvcrt");
              args.push_back ("/DEFAULTLIB:oldnames");
            }
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
          args.push_back ("/DEFAULTLIB:shell32");
          args.push_back ("/DEFAULTLIB:user32");

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

          // VC link.exe creates an import library and .exp file for an
          // executable if any of its object files export any symbols (think a
          // unit test linking libus{}). And, no, there is no way to suppress
          // it (but we can change their names with /IMPLIB). Well, there is a
          // way: create a .def file with an empty EXPORTS section, pass it to
          // lib.exe to create a dummy .exp (and .lib), and then pass this
          // empty .exp to link.exe. Wanna go this way? Didn't think so.
          //
          // Having no way to disable this, the next simplest thing seems to
          // be just cleaning this mess up. Note, however, that we better
          // change the default name since otherwise it will be impossible to
          // have a library and an executable with the same name in the same
          // directory (their .lib's will clash).
          //
          // Note also that if at some point we decide to support such "shared
          // executables" (-rdynamic, etc), then it will probably have to be a
          // different target type (exes{}?) since it will need a different set
          // of object files (-fPIC so probably objs{}), etc.
          //
          // Also, while we are at it, this means there could be a DLL without
          // an import library (which we currently don't handle very well).
          //
          out2 = "/IMPLIB:";

          if (ot == otype::s)
          {
            // On Windows libs{} is the DLL and an ad hoc group member is the
            // import library.
            //
            // This will also create the .exp export file. Its name will be
            // derived from the import library by changing the extension.
            // Lucky for us -- there is no option to name it.
            //
            out2 += reli.string ();
          }
          else
          {
            out2 += relt.string ();
            out2 += ".lib";
          }

          args.push_back (out2.c_str ());

          // If we have /DEBUG then name the .pdb file. It is an ad hoc group
          // member.
          //
          if (const char* o = find_option_prefix ("/DEBUG", args, true))
          {
            if (icasecmp (o, "/DEBUG:NONE") != 0)
            {
              const file& pdb (
                *find_adhoc_member<file> (t, *bs.find_target_type ("pdb")));

              out1 = "/PDB:";
              out1 += relative (pdb.path ()).string ();
              args.push_back (out1.c_str ());
            }
          }

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

              append_diag_color_options (args);

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
                  out = "-Wl,--out-implib=" + reli.string ();
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

      // For MinGW manifest is an object file.
      //
      if (!manifest.empty () && tsys == "mingw32")
        sargs.push_back (relative (manifest).string ());

      // LLD misses an input file if we are linking only whole archives (LLVM
      // bug #43744, fixed in 9.0.1, 10.0.0). Repeating one of the previously-
      // mentioned archives seems to work around the issue.
      //
      if (!seen_obj             &&
          !lt.static_library () &&
          tsys == "win32-msvc"  &&
          cast<string> (rs["bin.ld.id"]) == "msvc-lld")
      {
        uint64_t mj;
        if ((mj = cast<uint64_t> (rs["bin.ld.version.major"])) < 9 ||
            (mj == 9 &&
             cast<uint64_t> (rs["bin.ld.version.minor"]) == 0 &&
             cast<uint64_t> (rs["bin.ld.version.patch"]) == 0))
        {
          auto i (find_if (sargs.rbegin (), sargs.rend (),
                           [] (const string& a)
                           {
                             return a.compare (0, 14, "/WHOLEARCHIVE:") == 0;
                           }));

          if (i != sargs.rend ())
            sargs.push_back (i->c_str () + 14);
        }
      }

      // Shallow-copy sargs over to args.
      //
      append_args (sargs);

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

        auto rm = [&paths, this] (path&& m, const string&, bool interm)
        {
          if (!interm)
          {
            // Filter out paths that match one of the current paths or a
            // prefix of the real path (the latter takes care of auxiliary
            // things like .d, .t, etc., that are normally derived from the
            // target name).
            //
            // Yes, we are basically ad hoc-excluding things that break. Maybe
            // we should use something more powerful for the pattern, such as
            // regex? We could have a filesystem pattern which we then filter
            // against a regex pattern?
            //
            auto prefix = [&m] (const path& p)
            {
              return path::traits_type::compare (m.string (),
                                                 p.string (),
                                                 p.string ().size ()) == 0;
            };

            if (!prefix (*paths.real) &&
                m !=  paths.interm    &&
                m !=  paths.soname    &&
                m !=  paths.load      &&
                m !=  paths.link)
            {
              try_rmfile (m);

              if (m.extension () != "d")
              {
                try_rmfile (m + ".d");

                if (tsys == "win32-msvc")
                {
                  try_rmfile (m.base () += ".ilk");
                  try_rmfile (m += ".pdb");
                }
              }
            }
          }
          return true;
        };

        auto clean = [&rm] (const path& p)
        {
          try
          {
            if (verb >= 4) // Seeing this with -V doesn't really add any value.
              text << "rm " << p;

            // Note: doesn't follow symlinks.
            //
            path_search (p,
                         rm,
                         dir_path () /* start */,
                         path_match_flags::none);
          }
          catch (const system_error&) {} // Ignore errors.
        };

        if (!paths.clean_load.empty ())    clean (paths.clean_load);
        if (!paths.clean_version.empty ()) clean (paths.clean_version);
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

      // We have no choice but to serialize early if we want the command line
      // printed shortly before actually executing the linker. Failed that, it
      // may look like we are still executing in parallel.
      //
      scheduler::alloc_guard jobs_ag;
      if (!ctx.dry_run && cast_false<bool> (t[c_serialize]))
        jobs_ag = scheduler::alloc_guard (*ctx.sched, phase_unlock (nullptr));

      if (verb == 1)
        print_diag (lt.static_library () ? "ar" : "ld", t);
      else if (verb == 2)
        print_process (args);

      // Do any necessary fixups to the command line to make it runnable.
      //
      // Notice the split in the diagnostics: at verbosity level 1 we print
      // the "logical" command line while at level 2 and above -- what we are
      // actually executing.
      //
      // We also need to save the original for the diag_buffer::close() call
      // below if at verbosity level 1.
      //
      cstrings oargs;

      // Adjust linker parallelism.
      //
      // Note that we are not going to bother with oargs for this.
      //
      // Note also that we now have scheduler::serialize() which allows us to
      // block until full parallelism is available (this mode can currently
      // be forced with cc.serialize=true; maybe we should invent something
      // like config.cc.link_serialize or some such which can be used when
      // LTO is enabled).
      //
      string jobs_arg;

      if (!ctx.dry_run && !lt.static_library ())
      {
        switch (ctype)
        {
        case compiler_type::gcc:
          {
            // Rewrite -flto=auto (available since GCC 10).
            //
            // By default GCC 10 splits the optimization into 128 units.
            //
            if (cmaj < 10)
              break;

            auto i (find_option_prefix ("-flto", args.rbegin (), args.rend ()));
            if (i != args.rend () && strcmp (*i, "-flto=auto") == 0)
            {
              if (jobs_ag.n == 0) // Might already have (see above).
                jobs_ag = scheduler::alloc_guard (*ctx.sched, 0);

              jobs_arg = "-flto=" + to_string (1 + jobs_ag.n);
              *i = jobs_arg.c_str ();
            }
            break;
          }
        case compiler_type::clang:
          {
            // If we have -flto=thin and no explicit -flto-jobs=N (available
            // since Clang 4), then add our own -flto-jobs value.
            //
            if (cmaj < 4)
              break;

            auto i (find_option_prefix ("-flto", args.rbegin (), args.rend ()));
            if (i != args.rend ()              &&
                strcmp (*i, "-flto=thin") == 0 &&
                !find_option_prefix ("-flto-jobs=", args))
            {
              if (jobs_ag.n == 0) // Might already have (see above).
                jobs_ag = scheduler::alloc_guard (*ctx.sched, 0);

              jobs_arg = "-flto-jobs=" + to_string (1 + jobs_ag.n);
              args.insert (i.base (), jobs_arg.c_str ()); // After -flto=thin.
            }
            break;
          }
        case compiler_type::msvc:
        case compiler_type::icc:
          break;
        }
      }

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
        auto quote = [s = string ()] (const char* a) mutable -> const char*
        {
          return process::quote_argument (a, s, false /* batch */);
        };

        // Calculate the would-be command line length similar to how process'
        // implementation does it.
        //
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
            fail << "unable to write to " << f << ": " << e;
          }

          if (verb == 1)
            oargs = args;

          // Replace input arguments with @file.
          //
          targ = '@' + f.string ();
          args.resize (args_input);
          args.push_back (targ.c_str());
          args.push_back (nullptr);
        }
      }
#endif

      if (verb >= 3)
        print_process (args);

      // Remove the target file if any of the subsequent (after the linker)
      // actions fail or if the linker fails but does not clean up its mess
      // (like link.exe). If we don't do that, then we will end up with a
      // broken build that is up-to-date.
      //
      auto_rmfile rm;

      if (!ctx.dry_run)
      {
        rm = auto_rmfile (relt);

        try
        {
          // VC tools (both lib.exe and link.exe) send diagnostics to stdout.
          // Also, link.exe likes to print various gratuitous messages. So for
          // link.exe we filter that noise out.
          //
          // For lib.exe (and any other insane linker that may try to pull off
          // something like this) we are going to redirect stdout to stderr.
          // For sane compilers this should be harmless.
          //
          // Note that we don't need this for LLD's link.exe replacement which
          // is thankfully quiet.
          //
          bool filter (tsys == "win32-msvc"  &&
                       !lt.static_library () &&
                       cast<string> (rs["bin.ld.id"]) != "msvc-lld");

          process pr (*ld,
                      args,
                      0                                           /* stdin  */,
                      2                                           /* stdout */,
                      diag_buffer::pipe (ctx, filter /* force */) /* stderr */,
                      nullptr                                     /* cwd    */,
                      env_ptrs.empty () ? nullptr : env_ptrs.data ());

          diag_buffer dbuf (ctx, args[0], pr);

          if (filter)
            msvc_filter_link (dbuf, t, ot);

          dbuf.read ();

          {
            bool e (pr.wait ());

#ifdef _WIN32
            // Keep the options file if we have shown it.
            //
            if (!e && verb >= 3)
              trm.cancel ();
#endif

            dbuf.close (oargs.empty () ? args : oargs,
                        *pr.exit,
                        1 /* verbosity */);

            if (!e)
              throw failed ();
          }
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

        // Clean up executable's import library (see above for details). And
        // make sure we have an import library for a shared library.
        //
        if (tsys == "win32-msvc")
        {
          if (lt.executable ())
          {
            try_rmfile (relt + ".lib", true /* ignore_errors */);
            try_rmfile (relt + ".exp", true /* ignore_errors */);
          }
          else if (lt.shared_library ())
          {
            if (!file_exists (reli,
                              false /* follow_symlinks */,
                              true  /* ignore_error */))
              fail << "linker did not produce import library " << reli <<
                info << "perhaps this library does not export any symbols?";
          }
        }

        // Set executable bit on the .js file so that it can be run with a
        // suitable binfmt interpreter (e.g., nodejs). See Emscripten issue
        // 12707 for details.
        //
#ifndef _WIN32
        if (lt.executable () && tsys == "emscripten")
        {
          path_perms (relt,
                      (path_perms (relt) |
                       permissions::xu   |
                       permissions::xg   |
                       permissions::xo));
        }
#endif
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

        if (!ctx.dry_run)
        {
          run (ctx,
               rl,
               args,
               1 /* finish_verbosity */,
               env_ptrs.empty () ? nullptr : env_ptrs.data ());
        }
      }

      jobs_ag.deallocate ();

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
        auto ln = [&ctx] (const path& f, const path& l)
        {
          if (verb >= 3)
            text << "ln -sf " << f << ' ' << l;

          if (ctx.dry_run)
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
          if (!ctx.dry_run)
            touch (ctx, tp, false /* create */, verb_never);
        }
      }

      if (!ctx.dry_run)
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
    perform_clean (action a, const target& xt, match_data& md) const
    {
      const file& t (xt.as<file> ());

      ltype lt (link_type (t));

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

    const target* link_rule::
    import (const prerequisite_key& pk,
            const optional<string>&,
            const location&) const
    {
      tracer trace (x, "link_rule::import");

      // @@ TODO: do we want to make metadata loading optional?
      //
      optional<dir_paths> usr_lib_dirs;
      const target* r (search_library (nullopt /* action */,
                                       sys_lib_dirs, usr_lib_dirs,
                                       pk));

      if (r == nullptr)
        l4 ([&]{trace << "unable to find installed library " << pk;});

      return r;
    }
  }
}
