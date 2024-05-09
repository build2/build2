// file      : libbuild2/cc/common.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/common.hxx>

#include <libbuild2/file.hxx>        // import()
#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/cc/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // Recursively process prerequisite libraries of the specified library. If
    // proc_impl returns false, then only process interface (*.export.libs),
    // otherwise -- interface and implementation (prerequisite and from
    // *.libs, unless overriden with *.export.impl_libs).
    //
    // Note that here we assume that an interface library is also always an
    // implementation (since we don't use *.export.libs for static linking).
    // We currently have this restriction to make sure the target in
    // *.export.libs is up-to-date (which will happen automatically if it is
    // listed as a prerequisite of this library).
    //
    // Note that the order of processing is:
    //
    // 1. options (x.* then cc.* to be consistent with poptions/loptions)
    // 2. lib itself (if self is true)
    // 3. dependency libs (prerequisite_targets, left to right, depth-first)
    // 4. dependency libs (*.libs variables).
    //
    // If proc_opt_group is true, then pass to proc_opt the group rather than
    // the member if a member was picked (according to linfo) form a group.
    // This is useful when we only want to see the common options set on the
    // group.
    //
    // If either proc_opt or proc_lib return false, then any further
    // processing of this library or its dependencies is skipped. This can be
    // used to "prune" the graph traversal in case of duplicates. Note that
    // proc_opt is called twice for each library so carefully consider from
    // which to return false.
    //
    // The first argument to proc_lib is a pointer to the last element of an
    // array that contains the current library dependency chain all the way to
    // the library passed to process_libraries(). The first element of this
    // array is NULL. If this argument is NULL, then this is a library without
    // a target (e.g., -lm, -pthread, etc) and its name is in the second
    // argument (which could be resolved to an absolute path or passed as an
    // -l<name>/-pthread option). Otherwise, (the first argument is not NULL),
    // the second argument contains the target path (which can be empty in
    // case of the unknown DLL path).
    //
    // Initially, the second argument (library name) was a string (e.g., -lm)
    // but there are cases where the library is identified with multiple
    // options, such as -framework CoreServices (there are also cases like
    // -Wl,--whole-archive -lfoo -lbar -Wl,--no-whole-archive). So now it is a
    // vector_view that contains a fragment of options (from one of the *.libs
    // variables) that corresponds to the library (or several libraries, as in
    // the --whole-archive example above).
    //
    // Storing a reference to elements of library name in proc_lib is legal
    // (they come either from the target's path or from one of the *.libs
    // variables neither of which should change on this run).
    //
    // If proc_impl always returns false (that is, we are only interested in
    // interfaces), then top_li can be absent. This makes process_libraries()
    // not to pick the liba/libs{} member for installed libraries instead
    // passing the lib{} group itself. This can be used to match the semantics
    // of file_rule which, when matching prerequisites, does not pick the
    // liba/libs{} member (naturally) but just matches the lib{} group. Note
    // that currently this truly only works for installed lib{} since non-
    // installed ones don't have cc.type set. See proc_opt_group for an
    // alternative way to (potentially) achieve the desired semantics.
    //
    // Note that if top_li is present, then the target passed to proc_impl,
    // proc_lib, and proc_opt (unless proc_opt_group is true) is always a
    // file.
    //
    // The dedup argument is part of the interface dependency deduplication
    // functionality, similar to $x.deduplicate_export_libs(). Note, however,
    // that here we do it "properly" (i.e., using group members, etc).
    //
    void common::
    process_libraries (
      action a,
      const scope& top_bs,
      optional<linfo> top_li,
      const dir_paths& top_sysd,
      const mtime_target& l,     // liba/libs{}, libux{}, or lib{}
      bool la,
      lflags lf,
      const function<bool (const target&,
                           bool la)>& proc_impl,            // Implementation?
      const function<bool (const target* const*,            // Can be NULL.
                           const small_vector<reference_wrapper<
                             const string>, 2>&,            // Library "name".
                           lflags,                          // Link flags.
                           const string* type,              // whole cc.type
                           bool sys)>& proc_lib,            // System library?
      const function<bool (const target&,
                           const string& lang,              // lang from cc.type
                           bool com,                        // cc. or x.
                           bool exp)>& proc_opt,            // *.export.
      bool self,             // Call proc_lib on l?
      bool proc_opt_group,   // Call proc_opt on group instead of member?
      library_cache* cache) const
    {
      library_cache cache_storage;
      if (cache == nullptr)
        cache = &cache_storage;

      small_vector<const target*, 32> chain;

      if (proc_lib)
        chain.push_back (nullptr);

      process_libraries_impl (a, top_bs, top_li, top_sysd,
                              nullptr, l, la, lf,
                              proc_impl, proc_lib, proc_opt,
                              self, proc_opt_group,
                              cache, &chain, nullptr);
    }

    void common::
    process_libraries_impl (
      action a,
      const scope& top_bs,
      optional<linfo> top_li,
      const dir_paths& top_sysd,
      const target* lg,
      const mtime_target& l,
      bool la,
      lflags lf,
      const function<bool (const target&,
                           bool la)>& proc_impl,
      const function<bool (const target* const*,
                           const small_vector<reference_wrapper<
                             const string>, 2>&,
                           lflags,
                           const string* type,
                           bool sys)>& proc_lib,
      const function<bool (const target&,
                           const string& lang,
                           bool com,
                           bool exp)>& proc_opt,
      bool self,
      bool proc_opt_group,
      library_cache* cache,
      small_vector<const target*, 32>* chain,
      small_vector<const target*, 32>* dedup) const
    {
      // Add the library to the chain.
      //
      if (self && proc_lib)
      {
        if (find (chain->begin (), chain->end (), &l) != chain->end ())
          fail << "dependency cycle detected involving library " << l;

        chain->push_back (&l);
      }

      // We only lookup public variables so go straight for the public
      // variable pool.
      //
      auto& vp (top_bs.ctx.var_pool);

      do // Breakout loop.
      {
        // See what type of library this is (C, C++, etc). Use it do decide
        // which x.libs variable name to use. If it's unknown, then we only
        // look into prerequisites. Note: lookup starting from rule-specific
        // variables (target should already be matched). Note also that for
        // performance we use lookup_original() directly and only look in the
        // target (so no target type/pattern-specific).
        //
        const string* pt (
          cast_null<string> (
            l.state[a].lookup_original (c_type, true /* target_only */).first));

        // cc.type value format is <lang>[,...].
        //
        size_t p;
        const string& t (pt != nullptr
                         ? ((p = pt->find (',')) == string::npos
                            ? *pt
                            : string (*pt, 0, p))
                         : string ());

        // Why are we bothering with impl for binless libraries since all
        // their dependencies are by definition interface? Well, for one, it
        // could be that it is dynamically-binless (e.g., binless on some
        // platforms or in some configurations and binful on/in others). In
        // this case it would be helpful to have a uniform semantics so that,
        // for example, *.libs are used for liba{} regardless of whether it is
        // binless or not. On the other hand, having to specify both
        // *.export.libs=-lm and *.libs=-lm (or *.export.impl_libs) for an
        // always-binless library is sure not very intuitive. Not sure if we
        // can win here.
        //
        bool impl (proc_impl && proc_impl (l, la));
        bool cc (false), same (false);

        if (!t.empty ())
        {
          cc   = (t == "cc");
          same = (!cc && t == x);
        }

        const scope& bs (t.empty () || cc ? top_bs : l.base_scope ());

        lookup c_e_libs;
        lookup x_e_libs;

        if (!t.empty ())
        {
          // Note that we used to treat *.export.libs set on the liba/libs{}
          // members as *.libs overrides rather than as member-specific
          // interface dependencies. This difference in semantics proved to be
          // surprising so now we have separate *.export.impl_libs for that.
          // Note that in this case options come from *.export.* variables.
          //
          // Note also that we only check for *.*libs. If one doesn't have any
          // libraries but needs to set, say, *.loptions, then *.*libs should
          // be set to NULL or empty (this is why we check for the result
          // being defined).
          //
          // Note: for performance we call lookup_original() directly (we know
          // these variables are not overridable) and pass the base scope we
          // have already resolved.
          //
          // See also deduplicate_export_libs() if changing anything here.
          //
          {
            const variable& v (impl ? c_export_impl_libs : c_export_libs);
            c_e_libs = l.lookup_original (v, false, &bs).first;
          }

          if (!cc)
          {
            const variable& v (
              same
              ? (impl ? x_export_impl_libs : x_export_libs)
              : vp[t + (impl ? ".export.impl_libs" : ".export.libs")]);
            x_e_libs = l.lookup_original (v, false, &bs).first;
          }

          // Process options first.
          //
          if (proc_opt)
          {
            const target& ol (proc_opt_group && lg != nullptr ? *lg : l);

            // If all we know is it's a C-common library, then in both cases
            // we only look for cc.export.*.
            //
            if (cc)
            {
              if (!proc_opt (ol, t, true, true)) break;
            }
            else
            {
              if (impl)
              {
                // Interface and implementation: as discussed above, we can
                // have two situations: overriden export or default export.
                //
                if (c_e_libs.defined () || x_e_libs.defined ())
                {
                  // NOTE: should this not be from l.vars rather than l? Or
                  // perhaps we can assume non-common values will be set on
                  // libs{}/liba{}.
                  //
                  // Note: options come from *.export.* variables.
                  //
                  if (!proc_opt (ol, t, false, true) ||
                      !proc_opt (ol, t, true,  true)) break;
                }
                else
                {
                  // For default export we use the same options as were used
                  // to build the library.
                  //
                  if (!proc_opt (ol, t, false, false) ||
                      !proc_opt (ol, t, true,  false)) break;
                }
              }
              else
              {
                // Interface: only add *.export.* (interface dependencies).
                //
                if (!proc_opt (ol, t, false, true) ||
                    !proc_opt (ol, t, true,  true)) break;
              }
            }
          }
        }

        // Determine if an absolute path is to a system library. Note that
        // we assume both paths to be normalized.
        //
        auto sys = [] (const dir_paths& sysd, const string& p) -> bool
        {
          size_t pn (p.size ());

          for (const dir_path& d: sysd)
          {
            const string& ds (d.string ()); // Can be "/", otherwise no slash.
            size_t dn (ds.size ());

            if (pn > dn &&
                p.compare (0, dn, ds) == 0 &&
                (path::traits_type::is_separator (ds[dn - 1]) ||
                 path::traits_type::is_separator (p[dn])))
              return true;
          }

          return false;
        };

        // Next process the library itself if requested.
        //
        small_vector<reference_wrapper<const string>, 2> proc_lib_name;//Reuse.

        if (self && proc_lib)
        {
          // Note that while normally the path is assigned, in case of an
          // import stub the path to the DLL may not be known and so the path
          // will be empty (but proc_lib() will use the import stub).
          //
          const file* f;
          const path& p ((f = l.is_a<file> ()) ? f->path () : empty_path);

          bool s (pt != nullptr // If cc library (matched or imported).
                  ? cast_false<bool> (l.vars[c_system])
                  : !p.empty () && sys (top_sysd, p.string ()));

          proc_lib_name = {p.string ()};
          if (!proc_lib (&chain->back (), proc_lib_name, lf, pt, s))
            break;
        }

        optional<optional<linfo>> li;    // Calculate lazily.
        const dir_paths* sysd (nullptr); // Resolve lazily.

        // Find system search directories corresponding to this library, i.e.,
        // from its project and for its type (C, C++, etc).
        //
        auto find_sysd = [&top_sysd, &vp, t, cc, same, &bs, &sysd, this] ()
        {
          // Use the search dirs corresponding to this library scope/type.
          //
          sysd = (t.empty () || cc)
          ? &top_sysd // Imported library, use importer's sysd.
          : &cast<dir_paths> (
            bs.root_scope ()->vars[same
                                   ? x_sys_lib_dirs
                                   : vp[t + ".sys_lib_dirs"]]);
        };

        auto find_linfo = [top_li, t, cc, &bs, &l, &li] ()
        {
          li = (t.empty () || cc)
          ? top_li
          : optional<linfo> (link_info (bs, link_type (l).type)); // @@ PERF
        };

        // Only go into prerequisites (implementation) if instructed and we
        // are not using explicit export. Otherwise, interface dependencies
        // come from the lib{}:*.export.impl_libs below.
        //
        if (impl && !c_e_libs.defined () && !x_e_libs.defined ())
        {
#if 0
          assert (top_li); // Must pick a member if implementation (see above).
#endif

          for (const prerequisite_target& pt: l.prerequisite_targets[a])
          {
            // Note: adhoc prerequisites are not part of the library metadata
            // protocol (and we should check for adhoc first to avoid races
            // during execute).
            //
            if (pt.adhoc () || pt == nullptr)
              continue;

            if (marked (pt))
              fail << "implicit dependency cycle detected involving library "
                   << l;

            bool la;
            const file* f;

            if ((la = (f = pt->is_a<liba>  ())) ||
                (la = (f = pt->is_a<libux> ())) ||
                (      f = pt->is_a<libs>  ()))
            {
              // See link_rule for details.
              //
              const target* g ((pt.include & include_group) != 0
                               ? f->group
                               : nullptr);

              if (sysd == nullptr) find_sysd ();
              if (!li) find_linfo ();

              process_libraries_impl (a, bs, *li, *sysd,
                                      g, *f, la, pt.data /* lflags */,
                                      proc_impl, proc_lib, proc_opt,
                                      true /* self */, proc_opt_group,
                                      cache, chain, nullptr);
            }
          }
        }

        // Process libraries (recursively) from *.export.*libs (of type names)
        // handling import, etc.
        //
        // If it is not a C-common library, then it probably doesn't have any
        // of the *.libs.
        //
        if (!t.empty ())
        {
          optional<dir_paths> usrd; // Extract lazily.

          // Determine if a "simple path" is a system library.
          //
          auto sys_simple = [&sysd, &sys, &find_sysd] (const string& p) -> bool
          {
            bool s (!path::traits_type::absolute (p));

            if (!s)
            {
              if (sysd == nullptr) find_sysd ();
              s = sys (*sysd, p);
            }

            return s;
          };

          // Determine the length of the library name fragment as well as
          // whether it is a system library. Possible length values are:
          //
          // 1 - just the argument itself (-lm, -pthread)
          // 2 - argument and next element (-l m, -framework CoreServices)
          // 0 - unrecognized/until the end (-Wl,--whole-archive ...)
          //
          // See similar code in find_system_library().
          //
          auto sense_fragment = [&sys_simple, this] (const string& l) ->
            pair<size_t, bool>
          {
            size_t n;
            bool s (true);

            if (tsys == "win32-msvc")
            {
              if (l[0] == '/')
              {
                // Some other option (e.g., /WHOLEARCHIVE:<name>).
                //
                n = 0;
              }
              else
              {
                // Presumably a path.
                //
                n = 1;
                s = sys_simple (l);
              }
            }
            else
            {
              if (l[0] == '-')
              {
                // -l<name>, -l <name>, -pthread
                //
                if (l[1] == 'l' || l == "-pthread")
                {
                  n = l.size () == 2 ? 2 : 1;
                }
                // -framework <name> (Mac OS)
                //
                else if (tsys == "darwin" && l == "-framework")
                {
                  n = 2;
                }
                // Some other option (e.g., -Wl,--whole-archive).
                //
                else
                  n = 0;
              }
              else
              {
                // Presumably a path.
                //
                n = 1;
                s = sys_simple (l);
              }
            }

            return make_pair (n, s);
          };

          auto proc_intf = [&l, proc_opt_group, cache, chain,
                            &proc_impl, &proc_lib, &proc_lib_name, &proc_opt,
                            &sysd, &usrd,
                            &find_sysd, &find_linfo, &sense_fragment,
                            &bs, a, &li, impl, this] (
                              const lookup& lu,
                              small_vector<const target*, 32>* dedup,
                              size_t dedup_start) // Start of our deps.
          {
            const vector<name>* ns (cast_null<vector<name>> (lu));
            if (ns == nullptr || ns->empty ())
              return;

            for (auto i (ns->begin ()), e (ns->end ()); i != e; )
            {
              const name& n (*i);

              // Note: see also recursively-binless logic in link_rule if
              //       changing anything in simple library handling.
              //
              if (n.simple ())
              {
                // This is something like -lm or shell32.lib so should be a
                // valid path. But it can also be an absolute library path
                // (e.g., something that may come from our .{static/shared}.pc
                // files).
                //
                if (proc_lib)
                {
                  pair<size_t, bool> r (sense_fragment (n.value));

                  proc_lib_name.clear ();
                  for (auto e1 (r.first != 0 ? i + r.first : e);
                       i != e && i != e1 && i->simple ();
                       ++i)
                  {
                    proc_lib_name.push_back (i->value);
                  }

                  proc_lib (nullptr, proc_lib_name, 0, nullptr, r.second);
                  continue;
                }
              }
              else
              {
                // This is a potentially project-qualified target.
                //
                if (sysd == nullptr) find_sysd ();
                if (!li) find_linfo ();

                const mtime_target* t;
                const target* g;

                const char* w (nullptr);
                try
                {
                  pair<const mtime_target&, const target*> p (
                    resolve_library (a,
                                   bs,
                                     n,
                                     (n.pair ? (++i)->dir : dir_path ()),
                                     *li,
                                     *sysd, usrd,
                                     cache));

                  t = &p.first;
                  g = p.second;

                  // Deduplicate.
                  //
                  // Note that dedup_start makes sure we only consider our
                  // interface dependencies while maintaining the "through"
                  // list.
                  //
                  if (dedup != nullptr)
                  {
                    if (find (dedup->begin () + dedup_start,
                              dedup->end (),
                              t) != dedup->end ())
                    {
                      ++i;
                      continue;
                    }

                    dedup->push_back (t);
                  }
                }
                catch (const non_existent_library& e)
                {
                  // This is another manifestation of the "mentioned in
                  // *.export.libs but not in prerequisites" case (see below).
                  //
                  t = &e.target;
                  w = "unknown";
                }

                // This can happen if the target is mentioned in *.export.libs
                // (i.e., it is an interface dependency) but not in the
                // library's prerequisites (i.e., it is not an implementation
                // dependency).
                //
                // Note that we used to just check for path being assigned but
                // on Windows import-installed DLLs may legally have empty
                // paths.
                //
                if (w != nullptr)
                  ; // See above.
                else if (l.ctx.phase == run_phase::match)
                {
                  // We allow not matching installed libraries if all we need
                  // is their options (see compile_rule::apply()).
                  //
                  if (proc_lib || t->base_scope ().root_scope () != nullptr)
                  {
                    if (!t->matched (a))
                      w = "not matched";
                  }
                }
                else
                {
                  // Note that this check we only do if there is proc_lib
                  // (since it's valid to process library's options before
                  // updating it).
                  //
                  if (proc_lib)
                  {
                    if (t->mtime () == timestamp_unknown)
                      w = "out of date";
                  }
                }

                if (w != nullptr)
                {
                  fail   << (impl ? "implementation" : "interface")
                         << " dependency " << *t << " is " << w <<
                    info << "mentioned in *.export." << (impl ? "impl_" : "")
                         << "libs of target " << l <<
                    info << "is it a prerequisite of " << l << "?" << endf;
                }

                // Process it recursively.
                //
                bool u;
                bool la ((u = t->is_a<libux> ()) || t->is_a<liba> ());
                lflags lf (0);

                // If this is a static library, see if we need to link it
                // whole.
                //
                if (la && proc_lib)
                {
                  // Note: go straight for the public variable pool.
                  //
                  const variable& var (t->ctx.var_pool["bin.whole"]);

                  // See the link rule for the lookup semantics.
                  //
                  lookup l (
                    t->lookup_original (var, true /* target_only */).first);

                  if (l ? cast<bool> (*l) : u)
                    lf |= lflag_whole;
                }

                process_libraries_impl (
                  a, bs, *li, *sysd,
                  g, *t, la, lf,
                  proc_impl, proc_lib, proc_opt,
                  true /* self */, proc_opt_group,
                  cache, chain, dedup);
              }

              ++i;
            }
          };

          auto proc_intf_storage = [&proc_intf] (const lookup& lu1,
                                                 const lookup& lu2 = lookup ())
          {
            small_vector<const target*, 32> dedup_storage;

            if (lu1) proc_intf (lu1, &dedup_storage, 0);
            if (lu2) proc_intf (lu2, &dedup_storage, 0);
          };

          // Process libraries from *.libs (of type strings).
          //
          auto proc_impl = [&proc_lib, &proc_lib_name,
                            &sense_fragment] (const lookup& lu)
          {
            const strings* ns (cast_null<strings> (lu));
            if (ns == nullptr || ns->empty ())
              return;

            for (auto i (ns->begin ()), e (ns->end ()); i != e; )
            {
              // This is something like -lm or shell32.lib so should be a
              // valid path.
              //
              pair<size_t, bool> r (sense_fragment (*i));

              proc_lib_name.clear ();
              for (auto e1 (r.first != 0 ? i + r.first : e);
                   i != e && i != e1;
                   ++i)
              {
                proc_lib_name.push_back (*i);
              }

              proc_lib (nullptr, proc_lib_name, 0, nullptr, r.second);
            }
          };

          // Note: the same structure as when processing options above.
          //
          // If all we know is it's a C-common library, then in both cases we
          // only look for cc.export.*libs.
          //
          if (cc)
          {
            if (impl)
            {
              if (c_e_libs) proc_intf (c_e_libs, nullptr, 0);
            }
            else
            {
              if (c_e_libs)
              {
                if (dedup != nullptr)
                  proc_intf (c_e_libs, dedup, dedup->size ());
                else
                  proc_intf_storage (c_e_libs);
              }
            }
          }
          else
          {
            // Note: see also recursively-binless logic in link_rule if
            //       changing anything here.

            if (impl)
            {
              // Interface and implementation: as discussed above, we can have
              // two situations: overriden export or default export.
              //
              if (c_e_libs.defined () || x_e_libs.defined ())
              {
                // Why are we calling proc_intf() on *.impl_libs? Perhaps
                // because proc_impl() expects strings, not names? Yes, and
                // proc_intf() checks impl.
                //
                if (c_e_libs) proc_intf (c_e_libs, nullptr, 0);
                if (x_e_libs) proc_intf (x_e_libs, nullptr, 0);
              }
              else
              {
                // For default export we use the same options/libs as were
                // used to build the library. Since libraries in (non-export)
                // *.libs are not targets, we don't need to recurse.
                //
                // Note: for performance we call lookup_original() directly
                // (we know these variables are not overridable) and pass the
                // base scope we have already resolved.
                //
                if (proc_lib)
                {
                  const variable& v (same ? x_libs : vp[t + ".libs"]);
                  proc_impl (l.lookup_original (c_libs, false, &bs).first);
                  proc_impl (l.lookup_original (v, false, &bs).first);
                }
              }
            }
            else
            {
              // Interface: only add *.export.* (interface dependencies).
              //
              if (c_e_libs.defined () || x_e_libs.defined ())
              {
                if (dedup != nullptr)
                {
                  size_t s (dedup->size ()); // Start of our interface deps.

                  if (c_e_libs) proc_intf (c_e_libs, dedup, s);
                  if (x_e_libs) proc_intf (x_e_libs, dedup, s);
                }
                else
                  proc_intf_storage (c_e_libs, x_e_libs);
              }
            }
          }
        }
      } while (false); // Breakout loop end.

      // Remove this library from the chain.
      //
      if (self && proc_lib)
        chain->pop_back ();
    }

    // The name can be an absolute or relative target name (for example,
    // /tmp/libfoo/lib{foo} or ../libfoo/lib{foo}) or a project-qualified
    // relative target name (e.g., libfoo%lib{foo}).
    //
    // Note that in case of the relative target that comes from export.*libs,
    // the resolution happens relative to the base scope of the target from
    // which this export.*libs came, which is exactly what we want.
    //
    // Note that the scope, search paths, and the link order should all be
    // derived from the library target that mentioned this name. This way we
    // will select exactly the same target as the library's matched rule and
    // that's the only way to guarantee it will be up-to-date.
    //
    // If li is absent, then don't pick the liba/libs{} member, returning the
    // lib{} target itself. If li is present, then the returned target is
    // always a file. The second half of the returned pair is the group, if
    // the member was picked.
    //
    // Note: paths in sysd/usrd are expected to be absolute and normalized.
    //
    // Note: may throw non_existent_library.
    //
    pair<const mtime_target&, const target*> common::
    resolve_library (action a,
                     const scope& s,
                     const name& cn,
                     const dir_path& out,
                     optional<linfo> li,
                     const dir_paths& sysd,
                     optional<dir_paths>& usrd,
                     library_cache* cache) const
    {
      bool q (cn.qualified ());
      auto lo (li ? optional<lorder> (li->order) : nullopt);

      // If this is an absolute and normalized unqualified name (which means
      // the scope does not factor into the result), then first check the
      // cache.
      //
      // Note that normally we will have a handful of libraries repeated a
      // large number of times (see Boost for an extreme example of this).
      //
      // Note also that for non-utility libraries we know that only the link
      // order from linfo is used. While not caching it and always picking an
      // alternative could also work, we cache it to avoid the lookup.
      //
      if (cache != nullptr)
      {
        if (!q &&
            (cn.dir.absolute () && cn.dir.normalized ()) &&
            (out.empty () || (out.absolute () && out.normalized ())))
        {
          auto i (find_if (cache->begin (), cache->end (),
                           [lo, &cn, &out] (const library_cache_entry& e)
                           {
                             const target& t (e.lib);
                             return (e.lo == lo &&
                                     e.value == cn.value &&
                                     e.type == cn.type &&
                                     t.dir == cn.dir &&
                                     t.out == out);
                           }));

          if (i != cache->end ())
            return pair<const mtime_target&, const target*> {i->lib, i->group};
        }
        else
          cache = nullptr; // Do not cache.
      }

      if (cn.type != "lib" && cn.type != "liba" && cn.type != "libs")
        fail << "target name " << cn << " is not a library";

      const target* xt (nullptr);

      if (!q)
      {
        // Search for an existing target with this name "as if" it was a
        // prerequisite.
        //
        xt = search_existing (cn, s, out);

        if (xt == nullptr)
          fail << "unable to find library " << cn;
      }
      else
      {
        // This is import.
        //
        name n (cn), o; // Note: find_prerequisite_key() changes name.
        prerequisite_key pk (s.find_prerequisite_key (n, o, location ()));
        xt = search_library_existing (a, sysd, usrd, pk);

        if (xt == nullptr)
        {
          if (n.qualified ())
            xt = import_existing (s.ctx, pk);
        }

        if (xt == nullptr)
          fail << "unable to find library " << pk;
      }

      // If this is lib{}/libul{}, pick appropriate member unless we were
      // instructed not to.
      //
      const target* g (nullptr);
      if (li)
      {
        if (const libx* l = xt->is_a<libx> ())
        {
          g = xt;
          xt = link_member (*l, a, *li); // Pick lib*{e,a,s}{}.
        }
      }

      auto& t (xt->as<mtime_target> ());

      if (cache != nullptr)
        cache->push_back (library_cache_entry {lo, cn.type, cn.value, t, g});

      return pair<const mtime_target&, const target*> {t, g};
    }

    // Action should be absent if called during the load phase. Note that pk's
    // scope should not be NULL (even if dir is absolute).
    //
    // Note: paths in sysd/usrd are expected to be absolute and normalized.
    //
    // Note: see similar logic in find_system_library().
    //
    target* common::
    search_library (optional<action> act,
                    const dir_paths& sysd,
                    optional<dir_paths>& usrd,
                    const prerequisite_key& p,
                    bool exist) const
    {
      tracer trace (x, "search_library");

      assert (p.scope != nullptr && (!exist || act));

      context& ctx (p.scope->ctx);
      const scope& rs (*p.scope->root_scope ());

      // Here is the problem: we may be building for two different toolchains
      // simultaneously that use the same installed library. But our search is
      // toolchain-specific. To make sure we end up with different targets for
      // each toolchain we are going to "tag" each target with the linker path
      // as its out directory.
      //
      const process_path& ld (tsys != "win32-msvc"
                              ? cpath
                              : cast<process_path> (rs["bin.ld.path"]));

      // @@ This is hairy enough to warrant a separate implementation for
      //    Windows.

      // Note: since we are searching for a (presumably) installed library,
      // utility libraries do not apply.
      //
      bool l (p.is_a<lib> ());
      const optional<string>& ext (l ? nullopt : p.tk.ext); // Only liba/libs.

      // First figure out what we need to search for.
      //
      const string& name (*p.tk.name);

      // liba
      //
      path an;
      optional<string> ae;

      if (l || p.is_a<liba> ())
      {
        // We are trying to find a library in the search paths extracted from
        // the compiler. It would only be natural if we used the library
        // prefix/extension that correspond to this compiler and/or its
        // target.
        //
        // Unlike MinGW, VC's .lib/.dll.lib naming is by no means standard and
        // we might need to search for other names. In fact, there is no
        // reliable way to guess from the file name what kind of library it
        // is, static or import and we will have to do deep inspection of such
        // alternative names. However, if we did find .dll.lib, then we can
        // assume that .lib is the static library without any deep inspection
        // overhead.
        //
        const char* e ("");

        if (tsys == "win32-msvc")
        {
          an = path (name);
          e = "lib";
        }
        else
        {
          an = path ("lib" + name);
          e = "a";
        }

        ae = ext ? ext : string (e);
        if (!ae->empty ())
        {
          an += '.';
          an += *ae;
        }
      }

      // libs
      //
      path sn;
      optional<string> se;

      if (l || p.is_a<libs> ())
      {
        const char* e ("");

        if (tsys == "win32-msvc")
        {
          sn = path (name);
          e = "dll.lib";
        }
        else
        {
          sn = path ("lib" + name);

          if      (tsys == "darwin")  e = "dylib";
          else if (tsys == "mingw32") e = "dll.a"; // See search code below.
          else                        e = "so";
        }

        se = ext ? ext : string (e);
        if (!se->empty ())
        {
          sn += '.';
          sn += *se;
        }
      }

      // Now search.
      //
      liba* a (nullptr);
      libs* s (nullptr);

      pair<path, path> pc; // pkg-config .pc file paths.
      path f;              // Reuse the buffer.

      auto search =[&a, &s, &pc,
                    &an, &ae,
                    &sn, &se,
                    &name, ext,
                    &ld, &f,
                    &p, exist, &trace, this] (const dir_path& d) -> bool
      {
        context& ctx (p.scope->ctx);

        // Whether to look for a binless variant using the common .pc file
        // (see below).
        //
        // Normally we look for a binless version if the binful one was not
        // found. However, sometimes we may find what looks like a binful
        // library but on a closer examination realize that there is something
        // wrong with it (for example, it's not a Windows import library). In
        // such cases we want to omit looking for a binless library using the
        // common .pc file since it most likely corresponds to the binful
        // library (and we may end up in a infinite loop trying to resolve
        // itself).
        //
        bool ba (true);
        bool bs (true);

        timestamp mt;

        // libs
        //
        // Look for the shared library first. The order is important for VC:
        // only if we found .dll.lib can we safely assumy that just .lib is a
        // static library.
        //
        if (!sn.empty ())
        {
          f = d;
          f /= sn;
          mt = mtime (f);

          if (mt != timestamp_nonexistent)
          {
            // On Windows what we found is the import library which we need
            // to make the first ad hoc member of libs{}.
            //
            if (tclass == "windows")
            {
              libi* i (nullptr);
              insert_library (ctx, i, name, d, ld, se, exist, trace);

              ulock l (
                insert_library (ctx, s, name, d, ld, nullopt, exist, trace));

              if (!exist)
              {
                if (l.owns_lock ())
                {
                  s->adhoc_member = i; // We are first.
                  l.unlock ();
                }
                else
                  assert (find_adhoc_member<libi> (*s) == i);

                // Presumably there is a DLL somewhere, we just don't know
                // where (and its possible we might have to look for one if we
                // decide we need to do rpath emulation for installed
                // libraries as well). We will represent this as empty path
                // but valid timestamp (aka "trust me, it's there").
                //
                i->path_mtime (move (f), mt);
                s->path_mtime (path (), mt);
              }
            }
            else
            {
              insert_library (ctx, s, name, d, ld, se, exist, trace);
              s->path_mtime (move (f), mt);
            }
          }
          else if (!ext && tsys == "mingw32")
          {
            // Above we searched for the import library (.dll.a) but if it's
            // not found, then we also search for the .dll (unless the
            // extension was specified explicitly) since we can link to it
            // directly. Note also that the resulting libs{} would end up
            // being the .dll.
            //
            se = string ("dll");
            f = f.base (); // Remove .a from .dll.a.
            mt = mtime (f);

            if (mt != timestamp_nonexistent)
            {
              insert_library (ctx, s, name, d, ld, se, exist, trace);
              s->path_mtime (move (f), mt);
            }
          }
          else if (!ext && tsys == "darwin")
          {
            // Besides .dylib, Mac OS now also has "text-based stub libraries"
            // that use the .tbd extension. They appear to be similar to
            // Windows import libraries and contain information such as the
            // location of the .dylib library, its symbols, etc. For example,
            // there is /Library/.../MacOSX13.3.sdk/usr/lib/libsqlite3.tbd
            // which points to /usr/lib/libsqlite3.dylib (but which itself is
            // invisible/inaccessible, presumably for security).
            //
            // Note that for now we are treating the .tbd library as the
            // shared library but could probably do the more elaborate dance
            // with ad hoc members like on Windows if really necessary.
            //
            se = string ("tbd");
            f = f.base (); // Remove .dylib.
            f += ".tbd";
            mt = mtime (f);

            if (mt != timestamp_nonexistent)
            {
              insert_library (ctx, s, name, d, ld, se, exist, trace);
              s->path_mtime (move (f), mt);
            }
          }
        }

        // liba
        //
        // If we didn't find .dll.lib then we cannot assume .lib is static.
        //
        if (!an.empty () && (s != nullptr || tsys != "win32-msvc"))
        {
          f = d;
          f /= an;

          if ((mt = mtime (f)) != timestamp_nonexistent)
          {
            // Enter the target. Note that because the search paths are
            // normalized, the result is automatically normalized as well.
            //
            // Note that this target is outside any project which we treat
            // as out trees.
            //
            insert_library (ctx, a, name, d, ld, ae, exist, trace);
            a->path_mtime (move (f), mt);
          }
        }

        // Alternative search for VC.
        //
        if (tsys == "win32-msvc")
        {
          if (s == nullptr && !sn.empty ())
          {
            pair<libs*, bool> r (msvc_search_shared (ld, d, p, exist));

            if (r.first != nullptr)
              s = r.first;
            else if (!r.second)
              bs = false;
          }

          if (a == nullptr && !an.empty ())
          {
            pair<liba*, bool> r (msvc_search_static (ld, d, p, exist));

            if (r.first != nullptr)
              a = r.first;
            else if (!r.second)
              ba = false;
          }
        }

        // Look for binary-less libraries via pkg-config .pc files. Note that
        // it is possible we have already found one of them as binful but the
        // other is binless.
        //
        {
          bool na (a == nullptr && !an.empty ()); // Need static.
          bool ns (s == nullptr && !sn.empty ()); // Need shared.

          if (na || ns)
          {
            // Only consider the common .pc file if we can be sure there
            // is no binful variant.
            //
            pair<path, path> r (
              pkgconfig_search (d,
                                p.proj,
                                name,
                                na && ns && ba && bs /* common */));

            if (na && !r.first.empty ())
            {
              insert_library (ctx, a, name, d, ld, nullopt, exist, trace);
              a->path_mtime (path (), timestamp_unreal);
            }

            if (ns && !r.second.empty ())
            {
              insert_library (ctx, s, name, d, ld, nullopt, exist, trace);
              s->path_mtime (path (), timestamp_unreal);
            }

            // Only keep these .pc paths if we found anything via them.
            //
            if ((na && a != nullptr) || (ns && s != nullptr))
              pc = move (r);
          }
        }

        return a != nullptr || s != nullptr;
      };

      // First try user directories (i.e., -L or /LIBPATH).
      //
      bool sys (false);

      if (!usrd)
      {
        usrd = extract_library_search_dirs (*p.scope);

        // Handle automatic importing of installed build2 libraries. This is a
        // mirror side of the uninstalled case that is handled via the special
        // import.build2 value in import_search().
        //
        if (build_installed && p.proj && *p.proj == "build2")
        {
          // Check if import.build2 is set to NULL to disable relying on the
          // built-in path. We use this in our tests to make sure we are
          // importing and testing the build system being built and not the
          // one doing the building.
          //
          // Note that for the installed case this value is undefined by
          // default.
          //
          lookup l (rs[ctx.var_import_build2]);
          if (!(l.defined () && l->null))
          {
            // Note that we prepend it to other user directories instead of
            // making it the only one to allow things to be overriden (e.g.,
            // if build2 was moved or some such).
            //
            // Note: build_install_lib is already normalized.
            //
            usrd->insert (usrd->begin (), build_install_lib);
          }
        }
      }

      const dir_path* pd (nullptr);
      for (const dir_path& d: *usrd)
      {
        if (search (d))
        {
          pd = &d;
          break;
        }
      }

      // Next try system directories (i.e., those extracted from the compiler).
      //
      if (pd == nullptr)
      {
        for (const dir_path& d: sysd)
        {
          if (search (d))
          {
            pd = &d;
            break;
          }
        }

        sys = true;
      }

      if (pd == nullptr)
      {
        l5 ([&]{trace << "no library found for " << p;});
        return nullptr;
      }

      // Enter (or find) the lib{} target group.
      //
      lib* lt;
      insert_library (
        ctx, lt, name, *pd, ld, l ? p.tk.ext : nullopt, exist, trace);

      // Result.
      //
      target* r (l ? lt : (p.is_a<liba> () ? static_cast<target*> (a) : s));

      // Assume the rest is already done if existing.
      //
      if (exist)
        return r;

      // Try to extract library information from pkg-config. We only add the
      // default macro if we could not extract more precise information. The
      // idea is that in .pc files that we generate, we copy those macros (or
      // custom ones) from *.export.poptions.
      //
      // @@ Should we add .pc files as ad hoc members so pkgconfig_save() can
      // use their names when deriving -l-names (this would be especially
      // helpful for binless libraries to get hold of prefix/suffix, etc).
      //
      auto load_pc = [this, &trace,
                      act, &p, &name,
                      &sysd, &usrd,
                      pd, &pc, lt, a, s] (pair<bool, bool> metaonly)
      {
        l5 ([&]{trace << "loading pkg-config information during "
                      << (act ? "match" : "load") << " for "
                      << (a != nullptr ? "static " : "")
                      << (s != nullptr ? "shared " : "")
                      << "member(s) of " << *lt << "; metadata only: "
                      << metaonly.first << " " << metaonly.second;});

        // Add the "using static/shared library" macro (used, for example, to
        // handle DLL export). The absence of either of these macros would
        // mean some other build system that cannot distinguish between the
        // two (and no pkg-config information).
        //
        auto add_macro = [this] (target& t, const char* suffix)
        {
          // If there is already a value (either in cc.export or x.export),
          // don't add anything: we don't want to be accumulating defines nor
          // messing with custom values. And if we are adding, then use the
          // generic cc.export.
          //
          // The only way we could already have this value is if this same
          // library was also imported as a project (as opposed to installed).
          // Unlikely but possible. In this case the values were set by the
          // export stub and we shouldn't touch them.
          //
          if (!t.vars[x_export_poptions])
          {
            auto p (t.vars.insert (c_export_poptions));

            if (p.second)
            {
              // The "standard" macro name will be LIB<NAME>_{STATIC,SHARED},
              // where <name> is the target name. Here we want to strike a
              // balance between being unique and not too noisy.
              //
              string d ("-DLIB");

              d += sanitize_identifier (
                ucase (const_cast<const string&> (t.name)));

              d += '_';
              d += suffix;

              strings o;
              o.push_back (move (d));
              p.first = move (o);
            }
          }
        };

        if (pc.first.empty () && pc.second.empty ())
        {
          if (!pkgconfig_load (act, *p.scope,
                               *lt, a, s,
                               p.proj, name,
                               *pd, sysd, *usrd,
                               metaonly))
          {
            if (a != nullptr && !metaonly.first)  add_macro (*a, "STATIC");
            if (s != nullptr && !metaonly.second) add_macro (*s, "SHARED");
          }
        }
        else
          pkgconfig_load (act, *p.scope,
                          *lt, a, s,
                          pc,
                          *pd, sysd, *usrd,
                          metaonly);
      };

      // Mark as a "cc" library (unless already marked) and set the system
      // flag.
      //
      auto mark_cc = [sys, this] (target& t) -> bool
      {
        auto p (t.vars.insert (c_type));

        if (p.second)
        {
          p.first = string ("cc");

          if (sys)
            t.vars.assign (c_system) = true;
        }

        return p.second;
      };

      // Deal with the load phase case. The rest is already hairy enough so
      // let's not try to weave this logic into that.
      //
      if (!act)
      {
        assert (ctx.phase == run_phase::load);

        // The overall idea here is to set everything up so that the default
        // file_rule matches the returned targets, the same way as it would if
        // multiple operations were executed for the match phase (see below).
        //
        // Note however, that there is no guarantee that we won't end up in
        // the match phase code below even after loading things here. For
        // example, the same library could be searched from pkgconfig_load()
        // if specified as -l. And if we try to re-assign group members, then
        // that would be a race condition. So we use the cc mark to detect
        // this.
        //
        timestamp mt (timestamp_nonexistent);
        if (a != nullptr) {lt->a = a; a->group = lt; mt = a->mtime ();}
        if (s != nullptr) {lt->s = s; s->group = lt; mt = s->mtime ();}

        // @@ TODO: we currently always reload pkgconfig for lt (and below).
        //
        mark_cc (*lt);
        lt->mtime (mt); // Note: problematic, see below for details.

        // We can only load metadata from here since we can only do this
        // during the load phase. But it's also possible that a racing match
        // phase already found and loaded this library without metadata. So
        // looks like the only way is to load the metadata incrementally. We
        // can base this decision on the presense/absense of cc.type and
        // export.metadata.
        //
        pair<bool, bool> metaonly {false, false};

        if (a != nullptr && !mark_cc (*a))
        {
          if (a->vars[ctx.var_export_metadata])
            a = nullptr;
          else
            metaonly.first = true;
        }

        if (s != nullptr && !mark_cc (*s))
        {
          if (s->vars[ctx.var_export_metadata])
            s = nullptr;
          else
            metaonly.second = true;
        }

        // Try to extract library information from pkg-config.
        //
        if (a != nullptr || s != nullptr)
          load_pc (metaonly);

        return r;
      }

      // If we cannot acquire the lock then this mean the target has already
      // been matched and we assume all of this has already been done.
      //
      auto lock = [a = *act] (const target* t) -> target_lock
      {
        auto l (t != nullptr ? build2::lock (a, *t, true) : target_lock ());

        if (l && l.offset == target::offset_matched)
        {
          assert ((*t)[a].rule == &file_rule::rule_match);
          l.unlock ();
        }

        return l;
      };

      target_lock al (lock (a));
      target_lock sl (lock (s));

      target_lock ll (lock (lt));

      // Set lib{} group members to indicate what's available. Note that we
      // must be careful here since its possible we have already imported some
      // of its members.
      //
      timestamp mt (timestamp_nonexistent);
      if (ll)
      {
        // Mark the group since sometimes we use it itself instead of one of
        // the liba/libs{} members (see process_libraries_impl() for details).
        //
        // If it's already marked, then it could have been imported during
        // load (see above).
        //
        // @@ TODO: we currently always reload pkgconfig for lt (and above).
        //    Maybe pass NULL lt to pkgconfig_load() in this case?
        //
        if (mark_cc (*lt))
        {
          if (a != nullptr) {lt->a = a; mt = a->mtime ();}
          if (s != nullptr) {lt->s = s; mt = s->mtime ();}
        }
        else
          ll.unlock ();
      }

      if (!al) a = nullptr;
      if (!sl) s = nullptr;

      // If the library already has cc.type, then assume it was either already
      // imported (e.g., during load) or was matched by a rule.
      //
      if (a != nullptr && !mark_cc (*a)) a = nullptr;
      if (s != nullptr && !mark_cc (*s)) s = nullptr;

      if (a != nullptr) a->group = lt;
      if (s != nullptr) s->group = lt;

      if (ll && (a != nullptr || s != nullptr))
      {
        // Try to extract library information from pkg-config.
        //
        load_pc ({false, false} /* metaonly */);
      }

      // If we have the lock (meaning this is the first time), set the matched
      // rule. Failed that we will keep re-locking it, updating its members,
      // etc.
      //
      // For members, use the fallback file rule instead of noop since we may
      // need their prerequisites matched (used for modules support; see
      // pkgconfig_load(), search_modules() for details).
      //
      // Note also that these calls clear target data.
      //
      if (a != nullptr) match_rule (al, file_rule::rule_match);
      if (s != nullptr) match_rule (sl, file_rule::rule_match);
      if (ll)
      {
        // @@ Turns out this has a problem: file_rule won't match/execute
        //    group members. So what happens is that if we have two installed
        //    libraries, say lib{build2} that depends on lib{butl}, then
        //    lib{build2} will have lib{butl} as a prerequisite and file_rule
        //    that matches lib{build2} will update lib{butl} (also matched by
        //    file_rule), but not its members. Later, someone (for example,
        //    the newer() call in append_libraries()) will pick one of the
        //    members assuming it is executed and things will go sideways.
        //
        //    For now we hacked around the issue but the long term solution is
        //    probably to add to the bin module a special rule that is
        //    registered on the global scope and matches the installed lib{}
        //    targets. This rule will have to both update prerequisites like
        //    the file_rule and group members like the lib_rule (or maybe it
        //    can skip prerequisites since one of the member will do that; in
        //    which case maybe we will be able to reuse lib_rule maybe with
        //    the "all members" flag or some such). A few additional
        //    notes/thoughts:
        //
        //    - Will be able to stop inheriting lib{} from mtime_target.
        //
        //    - Will need to register for perform_update/clean like in context
        //      as well as for configure as in the config module (feels like
        //      shouldn't need to register for dist).
        //
        //    - Will need to test batches, immediate import thoroughly (this
        //      stuff is notoriously tricky to get right in all situations).
        //
        match_rule (ll, file_rule::rule_match);

        // Also bless the library group with a "trust me it exists" timestamp.
        // Failed that, if the rule match gets cleared (e.g., because of
        // multiple operations being executed), then the fallback file rule
        // won't match.
        //
        lt->mtime (mt);

        ll.unlock (); // Unlock group before members, for good measure.
      }

      return r;
    }

    void
    gcc_extract_library_search_dirs (const strings&, dir_paths&); // gcc.cxx

    void
    msvc_extract_library_search_dirs (const strings&, dir_paths&); // msvc.cxx

    dir_paths common::
    extract_library_search_dirs (const scope& bs) const
    {
      dir_paths r;

      // Extract user-supplied search paths (i.e., -L, /LIBPATH).
      //
      auto extract = [&bs, &r, this] (const value& val, const variable& var)
      {
        const auto& v (cast<strings> (val));

        auto df = make_diag_frame (
          [&var, &bs](const diag_record& dr)
          {
            dr << info << "in variable " << var << " for scope " << bs;
          });

        if (tsys == "win32-msvc")
          msvc_extract_library_search_dirs (v, r);
        else
          gcc_extract_library_search_dirs (v, r);
      };

      // Note that the compiler mode options are in sys_lib_dirs.
      //
      if (auto l = bs[c_loptions]) extract (*l, c_loptions);
      if (auto l = bs[x_loptions]) extract (*l, x_loptions);

      return r;
    }

    void common::
    append_diag_color_options (cstrings& args) const
    {
      switch (cclass)
      {
      case compiler_class::msvc:
        {
          // MSVC has the /diagnostics: option which has an undocumented value
          // `color`. It's unclear from which version of MSVC this value is
          // supported, but it works in 17.0, so let's start from there.
          //
          // Note that there is currently no way to disable color in the MSVC
          // diagnostics specifically (the /diagnostics:* option values are
          // cumulative and there doesn't seem to be a `color-` value). This
          // is probably not a big deal since one can just disable the color
          // globally (--no-diag-color).
          //
          // Note that clang-cl appears to use -fansi-escape-codes. See GH
          // issue #312 for background.
          //
          // Note that MSVC ignores /diagnostics:color if diagnostics is
          // written to a pipe. See GH issue #312 for details and a link to
          // the MSVC bug report.
          //
          if (show_diag_color ())
          {
            if (cvariant.empty () &&
                (cmaj > 19 || (cmaj == 19 && cmin >= 30)))
            {
              // Check for the prefix in case /diagnostics:color- gets added
              // eventually.
              //
              if (!find_option_prefixes ({"/diagnostics:color",
                                          "-diagnostics:color"}, args))
              {
                args.push_back ("/diagnostics:color");
              }
            }
          }

          break;
        }
      case compiler_class::gcc:
        {
          // Enable/disable diagnostics color unless a custom option is
          // specified.
          //
          // Supported from GCC 4.9 (8.1 on Windows) and (at least) from Clang
          // 3.5. Clang supports -f[no]color-diagnostics in addition to the
          // GCC's spelling. Note that to enable color on Windows Clang also
          // needs -fansi-escape-codes.
          //
          if (
#ifndef _WIN32
            ctype == compiler_type::gcc   ? cmaj > 4 || (cmaj == 4 && cmin >= 9) :
#else
            ctype == compiler_type::gcc   ? cmaj > 8 || (cmaj == 8 && cmin >= 1) :
#endif
            ctype == compiler_type::clang ? cmaj > 3 || (cmaj == 3 && cmin >= 5) :
            false)
          {
            if (!(find_option_prefix ("-fdiagnostics-color", args)        ||
                  find_option        ("-fno-diagnostics-color", args)     ||
                  find_option        ("-fdiagnostics-plain-output", args) ||
                  (ctype == compiler_type::clang &&
                   (find_option ("-fcolor-diagnostics", args) ||
                    find_option ("-fno-color-diagnostics", args)))))
            {
              // Omit -fno-diagnostics-color if stderr is not a terminal (we
              // know there will be no color in this case and the option will
              // just add noise, for example, in build logs).
              //
              if (const char* o = (
                    show_diag_color () ? "-fdiagnostics-color"    :
                    stderr_term        ? "-fno-diagnostics-color" :
                    nullptr))
              {
                args.push_back (o);

#ifdef _WIN32
                if (ctype == compiler_type::clang && o[2] != 'n')
                  args.push_back ("-fansi-escape-codes");
#endif
              }
            }
          }

          break;
        }
      }
    }
  }
}
