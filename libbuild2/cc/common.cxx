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
    // a target (e.g., -lpthread) and its name is in the second argument
    // (which could be resolved to an absolute path or passed as an -l<name>
    // option). Otherwise, (the first argument is not NULL), the second
    // argument contains the target path (which can be empty in case of the
    // unknown DLL path).
    //
    // Initially, the second argument (library name) was a string (e.g.,
    // -lpthread) but there are cases where the library is identified with
    // multiple options, such as -framework CoreServices (there are also cases
    // like -Wl,--whole-archive -lfoo -lbar -Wl,--no-whole-archive). So now it
    // is a vector_view that contains a fragment of options (from one of the
    // *.libs variables) that corresponds to the library (or several
    // libraries, as in the --whole-archive example above).
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
    // liba/libs{} member (naturally) but just matches the lib{} group.
    //
    // Note that if top_li is present, then the target passed to proc_impl,
    // proc_lib, and proc_opt is always a file.
    //
    void common::
    process_libraries (
      action a,
      const scope& top_bs,
      optional<linfo> top_li,
      const dir_paths& top_sysd,
      const mtime_target& l,                     // liba/libs{} or lib{}
      bool la,
      lflags lf,
      const function<bool (const target&,
                           bool la)>& proc_impl,            // Implementation?
      const function<bool (const target* const*,            // Can be NULL.
                           const small_vector<reference_wrapper<
                             const string>, 2>&,            // Library "name".
                           lflags,                          // Link flags.
                           const string* type,              // cc.type
                           bool sys)>& proc_lib,            // System library?
      const function<bool (const target&,
                           const string& type,              // cc.type
                           bool com,                        // cc. or x.
                           bool exp)>& proc_opt,            // *.export.
      bool self /*= false*/,                     // Call proc_lib on l?
      library_cache* cache,
      small_vector<const target*, 24>* chain) const
    {
      library_cache cache_storage;
      if (cache == nullptr)
        cache = &cache_storage;

      small_vector<const target*, 24> chain_storage;
      if (chain == nullptr)
      {
        chain = &chain_storage;

        if (proc_lib)
          chain->push_back (nullptr);
      }

      // Add the library to the chain.
      //
      if (self && proc_lib)
        chain->push_back (&l);

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
        const string* t (
          cast_null<string> (
            l.state[a].lookup_original (c_type, true /* target_only */).first));

        bool impl (proc_impl && proc_impl (l, la));
        bool cc (false), same (false);

        if (t != nullptr)
        {
          cc   = (*t == "cc");
          same = (!cc && *t == x);
        }

        const scope& bs (t == nullptr || cc ? top_bs : l.base_scope ());

        lookup c_e_libs;
        lookup x_e_libs;

        if (t != nullptr)
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
          // @@ PERF: do target_only (helps a bit in non-installed case)?
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
              : vp[*t + (impl ? ".export.impl_libs" : ".export.libs")]);
            x_e_libs = l.lookup_original (v, false, &bs).first;
          }

          // Process options first.
          //
          if (proc_opt)
          {
            // If all we know is it's a C-common library, then in both cases
            // we only look for cc.export.*.
            //
            if (cc)
            {
              if (!proc_opt (l, *t, true, true)) break;
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
                  if (!proc_opt (l, *t, false, true) ||
                      !proc_opt (l, *t, true,  true)) break;
                }
                else
                {
                  // For default export we use the same options as were used
                  // to build the library.
                  //
                  if (!proc_opt (l, *t, false, false) ||
                      !proc_opt (l, *t, true,  false)) break;
                }
              }
              else
              {
                // Interface: only add *.export.* (interface dependencies).
                //
                if (!proc_opt (l, *t, false, true) ||
                    !proc_opt (l, *t, true,  true)) break;
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

          bool s (t != nullptr // If cc library (matched or imported).
                  ? cast_false<bool> (l.vars[c_system])
                  : !p.empty () && sys (top_sysd, p.string ()));

          proc_lib_name = {p.string ()};
          if (!proc_lib (&chain->back (), proc_lib_name, lf, t, s))
            break;
        }

        optional<optional<linfo>> li;    // Calculate lazily.
        const dir_paths* sysd (nullptr); // Resolve lazily.

        // Find system search directories corresponding to this library, i.e.,
        // from its project and for its type (C, C++, etc).
        //
        auto find_sysd = [&top_sysd, t, cc, same, &bs, &sysd, this] ()
        {
          // Use the search dirs corresponding to this library scope/type.
          //
          sysd = (t == nullptr || cc)
          ? &top_sysd // Imported library, use importer's sysd.
          : &cast<dir_paths> (
            bs.root_scope ()->vars[same
                                   ? x_sys_lib_dirs
                                   : bs.ctx.var_pool[*t + ".sys_lib_dirs"]]);
        };

        auto find_linfo = [top_li, t, cc, &bs, &l, &li] ()
        {
          li = (t == nullptr || cc)
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
            // protocol (and we should check for adhoc first to avoid races).
            //
            if (pt == nullptr || pt.adhoc ())
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
              if (sysd == nullptr) find_sysd ();
              if (!li) find_linfo ();

              process_libraries (a, bs, *li, *sysd,
                                 *f, la, pt.data,
                                 proc_impl, proc_lib, proc_opt, true,
                                 cache, chain);
            }
          }
        }

        // Process libraries (recursively) from *.export.*libs (of type names)
        // handling import, etc.
        //
        // If it is not a C-common library, then it probably doesn't have any
        // of the *.libs.
        //
        if (t != nullptr)
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
          // 1 - just the argument itself (-lpthread)
          // 2 - argument and next element (-l pthread, -framework CoreServices)
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
                // -l<name>, -l <name>
                //
                if (l[1] == 'l')
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

          auto proc_int = [&l, cache, chain,
                           &proc_impl, &proc_lib, &proc_lib_name, &proc_opt,
                           &sysd, &usrd,
                           &find_sysd, &find_linfo, &sense_fragment,
                           &bs, a, &li, impl, this] (const lookup& lu)
          {
            const vector<name>* ns (cast_null<vector<name>> (lu));
            if (ns == nullptr || ns->empty ())
              return;

            for (auto i (ns->begin ()), e (ns->end ()); i != e; )
            {
              const name& n (*i);

              if (n.simple ())
              {
                // This is something like -lpthread or shell32.lib so should
                // be a valid path. But it can also be an absolute library
                // path (e.g., something that may come from our
                // .{static/shared}.pc files).
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

                const mtime_target& t (
                  resolve_library (a,
                                   bs,
                                   n,
                                   (n.pair ? (++i)->dir : dir_path ()),
                                   *li,
                                   *sysd, usrd,
                                   cache));

                if (proc_lib)
                {
                  // This can happen if the target is mentioned in
                  // *.export.libs (i.e., it is an interface dependency) but
                  // not in the library's prerequisites (i.e., it is not an
                  // implementation dependency).
                  //
                  // Note that we used to just check for path being assigned
                  // but on Windows import-installed DLLs may legally have
                  // empty paths.
                  //
                  const char* w (nullptr);
                  if (t.ctx.phase == run_phase::match)
                  {
                    size_t o (
                      t.state[a].task_count.load (memory_order_consume) -
                      t.ctx.count_base ());

                    if (o != target::offset_applied &&
                        o != target::offset_executed)
                      w = "not matched";
                  }
                  else if (t.mtime () == timestamp_unknown)
                    w = "out of date";

                  if (w != nullptr)
                    fail   << (impl ? "implementation" : "interface")
                           << " dependency " << t << " is " << w <<
                      info << "mentioned in *.export." << (impl ? "impl_" : "")
                           << "libs of target " << l <<
                      info << "is it a prerequisite of " << l << "?";
                }

                // Process it recursively.
                //
                // @@ Where can we get the link flags? Should we try to find
                //    them in the library's prerequisites? What about
                //    installed stuff?
                //
                process_libraries (a, bs, *li, *sysd,
                                   t, t.is_a<liba> () || t.is_a<libux> (), 0,
                                   proc_impl, proc_lib, proc_opt, true,
                                   cache, chain);
              }

              ++i;
            }
          };

          // Process libraries from *.libs (of type strings).
          //
          auto proc_imp = [&proc_lib, &proc_lib_name,
                           &sense_fragment] (const lookup& lu)
          {
            const strings* ns (cast_null<strings> (lu));
            if (ns == nullptr || ns->empty ())
              return;

            for (auto i (ns->begin ()), e (ns->end ()); i != e; )
            {
              // This is something like -lpthread or shell32.lib so should be
              // a valid path.
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
            if (c_e_libs) proc_int (c_e_libs);
          }
          else
          {
            if (impl)
            {
              // Interface and implementation: as discussed above, we can have
              // two situations: overriden export or default export.
              //
              if (c_e_libs.defined () || x_e_libs.defined ())
              {
                if (c_e_libs) proc_int (c_e_libs);
                if (x_e_libs) proc_int (x_e_libs);
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
                  const variable& v (same ? x_libs : vp[*t + ".libs"]);
                  proc_imp (l.lookup_original (c_libs, false, &bs).first);
                  proc_imp (l.lookup_original (v, false, &bs).first);
                }
              }
            }
            else
            {
              // Interface: only add *.export.* (interface dependencies).
              //
              if (c_e_libs) proc_int (c_e_libs);
              if (x_e_libs) proc_int (x_e_libs);
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
    // always a file.
    //
    const mtime_target& common::
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
      // order from linfo is used.
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
            return i->lib;
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

      // If this is lib{}/libu*{}, pick appropriate member unless we were
      // instructed not to.
      //
      if (li)
      {
        if (const libx* l = xt->is_a<libx> ())
          xt = link_member (*l, a, *li); // Pick lib*{e,a,s}{}.
      }

      auto& t (xt->as<mtime_target> ());

      if (cache != nullptr)
        cache->push_back (library_cache_entry {lo, cn.type, cn.value, t});

      return t;
    }

    // Note that pk's scope should not be NULL (even if dir is absolute).
    //
    // Note: see similar logic in find_system_library().
    //
    target* common::
    search_library (action act,
                    const dir_paths& sysd,
                    optional<dir_paths>& usrd,
                    const prerequisite_key& p,
                    bool exist) const
    {
      tracer trace (x, "search_library");

      assert (p.scope != nullptr);

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
            s = msvc_search_shared (ld, d, p, exist);

          if (a == nullptr && !an.empty ())
            a = msvc_search_static (ld, d, p, exist);
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
              pkgconfig_search (d, p.proj, name, na && ns /* common */));

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

      // If we cannot acquire the lock then this mean the target has already
      // been matched and we assume all of this has already been done.
      //
      auto lock = [act] (const target* t) -> target_lock
      {
        auto l (t != nullptr ? build2::lock (act, *t, true) : target_lock ());

        if (l && l.offset == target::offset_matched)
        {
          assert ((*t)[act].rule == &file_rule::rule_match);
          l.unlock ();
        }

        return l;
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

      target_lock ll (lock (lt));

      // Set lib{} group members to indicate what's available. Note that we
      // must be careful here since its possible we have already imported some
      // of its members.
      //
      timestamp mt (timestamp_nonexistent);
      if (ll)
      {
        if (s != nullptr) {lt->s = s; mt = s->mtime ();}
        if (a != nullptr) {lt->a = a; mt = a->mtime ();}

        // Mark the group since sometimes we use it itself instead of one of
        // the liba/libs{} members (see process_libraries() for details).
        //
        mark_cc (*lt);
      }

      target_lock al (lock (a));
      target_lock sl (lock (s));

      if (!al) a = nullptr;
      if (!sl) s = nullptr;

      if (a != nullptr) a->group = lt;
      if (s != nullptr) s->group = lt;

      // If the library already has cc.type, then assume it was either
      // already imported or was matched by a rule.
      //
      if (a != nullptr && !mark_cc (*a)) a = nullptr;
      if (s != nullptr && !mark_cc (*s)) s = nullptr;

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

      if (ll && (a != nullptr || s != nullptr))
      {
        // Try to extract library information from pkg-config. We only add the
        // default macro if we could not extract more precise information. The
        // idea is that in .pc files that we generate, we copy those macros
        // (or custom ones) from *.export.poptions.
        //
        // @@ Should we add .pc files as ad hoc members so pkconfig_save() can
        // use their names when deriving -l-names (this would be expecially
        // helpful for binless libraries to get hold of prefix/suffix, etc).
        //
        if (pc.first.empty () && pc.second.empty ())
        {
          if (!pkgconfig_load (act, *p.scope,
                               *lt, a, s,
                               p.proj, name,
                               *pd, sysd, *usrd,
                               false /* metadata */))
          {
            if (a != nullptr) add_macro (*a, "STATIC");
            if (s != nullptr) add_macro (*s, "SHARED");
          }
        }
        else
          pkgconfig_load (act, *p.scope,
                          *lt, a, s,
                          pc,
                          *pd, sysd, *usrd,
                          false /* metadata */);
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
      if (al) match_rule (al, file_rule::rule_match);
      if (sl) match_rule (sl, file_rule::rule_match);
      if (ll)
      {
        match_rule (ll, file_rule::rule_match);

        // Also bless the library group with a "trust me it exists" timestamp.
        // Failed that, if the rule match gets cleared (e.g., because of
        // multiple operations being executed), then the fallback file rule
        // won't match.
        //
        lt->mtime (mt);
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
  }
}
