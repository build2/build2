// file      : libbuild2/cc/functions.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/link-rule.hxx>
#include <libbuild2/cc/compile-rule.hxx>

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/bin/target.hxx>
#include <libbuild2/bin/utility.hxx>

#include <libbuild2/cc/module.hxx>
#include <libbuild2/cc/utility.hxx>

#include <libbuild2/functions-name.hxx> // to_target()

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // Common thunk for $x.*(<targets> [, ...]) functions.
    //
    struct thunk_data
    {
      const char* x;
      void (*f) (strings&,
                 const vector_view<value>&, const module&, const scope&,
                 action, const target&);
    };

    static value
    thunk (const scope* bs,
           vector_view<value> vs,
           const function_overload& f)
    {
      const auto& d (*reinterpret_cast<const thunk_data*> (&f.data));

      if (bs == nullptr)
        fail << f.name << " called out of scope";

      const scope* rs (bs->root_scope ());

      if (rs == nullptr)
        fail << f.name << " called out of project";

      // Note that we also allow calling this during match since an ad hoc
      // recipe with dynamic dependency extraction (depdb-dyndep) executes its
      // depdb preamble during match (after matching all the prerequisites).
      //
      if (bs->ctx.phase != run_phase::match &&
          bs->ctx.phase != run_phase::execute)
        fail << f.name << " can only be called from recipe";

      const module* m (rs->find_module<module> (d.x));

      if (m == nullptr)
        fail << f.name << " called without " << d.x << " module loaded";

      // We can assume these are present due to function's types signature.
      //
      if (vs[0].null)
        throw invalid_argument ("null value");

      names& ts_ns (vs[0].as<names> ()); // <targets>

      // In a somewhat hackish way strip the outer operation to match how we
      // call the underlying functions in the compile/link rules. This should
      // be harmless since ad hoc recipes are always for the inner operation.
      //
      action a (rs->ctx.current_action ().inner_action ());

      strings r;
      for (auto i (ts_ns.begin ()); i != ts_ns.end (); ++i)
      {
        name& n (*i), o;
        const target& t (to_target (*bs, move (n), move (n.pair ? *++i : o)));

        if (!t.matched (a))
          fail << t << " is not matched" <<
            info << "make sure this target is listed as prerequisite";

        d.f (r, vs, *m, *bs, a, t);
      }

      return value (move (r));
    }

    // Common thunk for $x.lib_*(...) functions.
    //
    // The two supported function signatures are:
    //
    // $x.lib_*(<targets>, <otype> [, ...]])
    //
    // $x.lib_*(<targets>)
    //
    // For the first signature, the passed targets cannot be library groups
    // (so they are always file-based) and linfo is always present.
    //
    // For the second signature, targets can only be utility libraries
    // (including the libul{} group).
    //
    // If <otype> in the first signature is NULL, then it is treated as
    // the second signature.
    //
    struct lib_thunk_data
    {
      const char* x;
      void (*f) (void*, strings&,
                 const vector_view<value>&, const module&, const scope&,
                 action, const target&, bool, optional<linfo>);
    };

    static value
    lib_thunk_impl (void* ls,
                    const scope* bs,
                    vector_view<value> vs,
                    const function_overload& f)
    {
      const auto& d (*reinterpret_cast<const lib_thunk_data*> (&f.data));

      if (bs == nullptr)
        fail << f.name << " called out of scope";

      const scope* rs (bs->root_scope ());

      if (rs == nullptr)
        fail << f.name << " called out of project";

      if (bs->ctx.phase != run_phase::match && // See above.
          bs->ctx.phase != run_phase::execute)
        fail << f.name << " can only be called from recipe";

      const module* m (rs->find_module<module> (d.x));

      if (m == nullptr)
        fail << f.name << " called without " << d.x << " module loaded";

      // We can assume this is present due to function's types signature.
      //
      if (vs[0].null)
        throw invalid_argument ("null value");

      names& ts_ns (vs[0].as<names> ()); // <targets>

      optional<linfo> li;
      if (vs.size () > 1 && !vs[1].null)
      {
        names& ot_ns (vs[1].as<names> ()); // <otype>

        string t (convert<string> (move (ot_ns)));

        const target_type* tt (bs->find_target_type (t));

        if (tt == nullptr)
          fail << "unknown target type '" << t << "'";

        // Try both linker and compiler output types.
        //
        otype ot (link_type (*tt).type);

        switch (ot)
        {
        case otype::e:
        case otype::a:
        case otype::s:
          break;
        default:
          ot = compile_type (*tt);
          switch (ot)
          {
          case otype::e:
          case otype::a:
          case otype::s:
            break;
          default:
            fail << "target type " << t << " is not compiler/linker output";
          }
        }

        li = link_info (*bs, ot);
      }

      // In a somewhat hackish way strip the outer operation to match how we
      // call the underlying functions in the compile/link rules. This should
      // be harmless since ad hoc recipes are always for the inner operation.
      //
      action a (rs->ctx.current_action ().inner_action ());

      strings r;
      for (auto i (ts_ns.begin ()); i != ts_ns.end (); ++i)
      {
        name& n (*i), o;
        const target& t (to_target (*bs, move (n), move (n.pair ? *++i : o)));

        bool la (false);
        if (li
            ? ((la = t.is_a<libux> ()) ||
               (la = t.is_a<liba>  ()) ||
               (     t.is_a<libs>  ()))
            : ((la = t.is_a<libux> ()) ||
               (     t.is_a<libul>  ())))
        {
          if (!t.matched (a))
            fail << t << " is not matched" <<
              info << "make sure this target is listed as prerequisite";

          d.f (ls, r, vs, *m, *bs, a, t, la, li);
        }
        else
          fail << t << " is not a library of expected type";
      }

      return value (move (r));
    }

    template <typename L>
    static value
    lib_thunk (const scope* bs,
               vector_view<value> vs,
               const function_overload& f)
    {
      L ls;
      return lib_thunk_impl (&ls, bs, vs, f);
    }

    // @@ Maybe we should provide wrapper functions that return all the
    //    compile options (including from *.?options, mode, etc) and all the
    //    link arguments in the correct order, etc. Can call them:
    //
    //    compile_options()
    //    link_arguments()
    //

    void compile_rule::
    functions (function_family& f, const char* x)
    {
      // $<module>.lib_poptions(<lib-targets>[, <otype>[, <original>]])
      //
      // Return the preprocessor options that should be passed when compiling
      // sources that depend on the specified libraries. The second argument
      // is the output target type (obje, objs, etc).
      //
      // The output target type may be omitted for utility libraries (libul{}
      // or libu[eas]{}). In this case, only "common interface" options will
      // be returned for lib{} dependencies. This is primarily useful for
      // obtaining poptions to be passed to tools other than C/C++ compilers
      // (for example, Qt moc).
      //
      // If <original> is true, then return the original -I options without
      // performing any translation (for example, to -isystem or /external:I).
      // This is the default if <otype> is omitted. To get the translation for
      // the common interface options, pass [null] for <otype> and true for
      // <original>.
      //
      // Note that passing multiple targets at once is not a mere convenience:
      // this also allows for more effective duplicate suppression.
      //
      // Note also that this function can only be called during execution (or,
      // carefully, during match) after all the specified library targets have
      // been matched. Normally it is used in ad hoc recipes to implement
      // custom compilation.
      //
      // Note that this function is not pure.
      //
      f.insert (".lib_poptions", false).
        insert<lib_thunk_data, names, optional<names*>, optional<names>> (
        &lib_thunk<appended_libraries>,
        lib_thunk_data {
          x,
          [] (void* ls, strings& r,
              const vector_view<value>& vs, const module& m, const scope& bs,
              action a, const target& l, bool la, optional<linfo> li)
          {
            // If this is libul{}, get the matched member (see bin::libul_rule
            // for details).
            //
            const file& f (
              la || li
              ? l.as<file> ()
              : (la = true,
                 l.prerequisite_targets[a].back ().target->as<file> ()));

            bool common (!li);
            bool original (vs.size () > 2 ? convert<bool> (vs[2]) : !li);

            if (!li)
              li = link_info (bs, link_type (f).type);

            m.append_library_options (
              *static_cast<appended_libraries*> (ls), r,
              bs, a, f, la, *li, common, original);
          }});

      // $<module>.find_system_header(<name>)
      //
      // Return the header path if the specified header exists in one of the
      // system header search directories and NULL otherwise. System header
      // search directories are those that the compiler searches by default
      // plus directories specified as part of the compiler mode options (but
      // not *.poptions).
      //
      // Note that this function is not pure.
      //
      f.insert (".find_system_header", false).
        insert<const char*, names> (
          [] (const scope* bs,
              vector_view<value> vs,
              const function_overload& f) -> value
          {
            const char* x (*reinterpret_cast<const char* const*> (&f.data));

            if (bs == nullptr)
              fail << f.name << " called out of scope";

            const scope* rs (bs->root_scope ());

            if (rs == nullptr)
              fail << f.name << " called out of project";

            const module* m (rs->find_module<module> (x));

            if (m == nullptr)
              fail << f.name << " called without " << x << " module loaded";

            // We can assume the argument is present due to function's types
            // signature.
            //
            auto r (m->find_system_header (convert<path> (move (vs[0]))));
            return r ? value (move (*r)) : value (nullptr);
          },
          x);
    }

    void link_rule::
    functions (function_family& f, const char* x)
    {
      // $<module>.lib_libs(<lib-targets>, <otype> [, <flags> [, <self>]])
      //
      // Return the libraries (and any associated options) that should be
      // passed when linking targets that depend on the specified libraries.
      // The second argument is the output target type (exe, libs, etc).
      //
      // The following flags are supported:
      //
      // whole    - link the specified libraries in the whole archive mode
      //
      // absolute - return absolute paths to the libraries
      //
      // If the last argument is false, then do not return the specified
      // libraries themselves.
      //
      // Note that passing multiple targets at once is not a mere convenience:
      // this also allows for more effective duplicate suppression.
      //
      // Note also that this function can only be called during execution (or,
      // carefully, during match) after all the specified library targets have
      // been matched. Normally it is used in ad hoc recipes to implement
      // custom linking.
      //
      // Note that this function is not pure.
      //
      f.insert (".lib_libs", false).
        insert<lib_thunk_data, names, names, optional<names>, optional<names>> (
        &lib_thunk<appended_libraries>,
        lib_thunk_data {
          x,
          [] (void* ls, strings& r,
              const vector_view<value>& vs, const module& m, const scope& bs,
              action a, const target& l, bool la, optional<linfo> li)
          {
            lflags lf (0);
            bool rel (true);
            if (vs.size () > 2)
            {
              if (vs[2].null)
                throw invalid_argument ("null value");

              for (const name& f: vs[2].as<names> ())
              {
                string s (convert<string> (name (f)));

                if (s == "whole")
                  lf |= lflag_whole;
                else if (s == "absolute")
                  rel = false;
                else
                  fail << "invalid flag '" << s << "'";
              }
            }

            bool self (vs.size () > 3 ? convert<bool> (vs[3]) : true);

            m.append_libraries (
              *static_cast<appended_libraries*> (ls), r,
              nullptr /* sha256 */, nullptr /* update */, timestamp_unknown,
              bs, a, l.as<file> (), la, lf, *li,
              nullopt /* for_install */, self, rel);
          }});

      // $<module>.lib_rpaths(<lib-targets>, <otype> [, <link> [, <self>]])
      //
      // Return the rpath options that should be passed when linking targets
      // that depend on the specified libraries. The second argument is the
      // output target type (exe, libs, etc).
      //
      // If the third argument is true, then use rpath-link options rather
      // than rpath (which is what should normally be used when linking for
      // install, for example).
      //
      // If the last argument is false, then do not return the options for the
      // specified libraries themselves.
      //
      // Note that passing multiple targets at once is not a mere convenience:
      // this also allows for more effective duplicate suppression.
      //
      // Note also that this function can only be called during execution
      // after all the specified library targets have been matched. Normally
      // it is used in ad hoc recipes to implement custom linking.
      //
      // Note that this function is not pure.
      //
      f.insert (".lib_rpaths", false).
        insert<lib_thunk_data, names, names, optional<names>, optional<names>> (
        &lib_thunk<rpathed_libraries>,
        lib_thunk_data {
          x,
          [] (void* ls, strings& r,
              const vector_view<value>& vs, const module& m, const scope& bs,
              action a, const target& l, bool la, optional<linfo> li)
          {
            bool link (vs.size () > 2 ? convert<bool> (vs[2]) : false);
            bool self (vs.size () > 3 ? convert<bool> (vs[3]) : true);
            m.rpath_libraries (*static_cast<rpathed_libraries*> (ls), r,
                               bs, a, l.as<file> (), la, *li, link, self);
          }});

      // $cxx.obj_modules(<obj-targets>)
      //
      // Return object files corresponding to module interfaces that are used
      // by the specified object files and that belong to binless libraries.
      //
      // Note that passing multiple targets at once is not a mere convenience:
      // this also allows for more effective duplicate suppression.
      //
      // Note also that this function can only be called during execution
      // after all the specified object file targets have been matched.
      // Normally it is used in ad hoc recipes to implement custom linking.
      //
      // Note that this function is not pure.
      //
      f.insert (".obj_modules", false).
        insert<thunk_data, names> (
        &thunk,
        thunk_data {
          x,
          [] (strings& r,
              const vector_view<value>&, const module& m, const scope& bs,
              action a, const target& t)
          {
            if (const file* f = t.is_a<objx> ())
            {
              if (m.modules)
                m.append_binless_modules (r, nullptr /* sha256 */, bs, a, *f);
            }
            else
              fail << t << " is not an object file target";
          }});

      // $<module>.deduplicate_export_libs(<names>)
      //
      // Deduplicate interface library dependencies by removing libraries that
      // are also interface dependencies of the specified libraries. This can
      // result in significantly better build performance for heavily
      // interface-interdependent library families (for example, like Boost).
      // Typical usage:
      //
      // import intf_libs  = ...
      // import intf_libs += ...
      // ...
      // import intf_libs += ...
      // intf_libs = $cxx.deduplicate_export_libs($intf_libs)
      //
      // Notes:
      //
      // 1. We only consider unqualified absolute/normalized target names (the
      //    idea is that the installed case will already be deduplicated).
      //
      // 2. We assume all the libraries listed are of the module type and only
      //    look for cc.export.libs and <module>.export.libs.
      //
      // 3. No member/group selection/linkup: we resolve *.export.libs on
      //    whatever is listed (so no liba{}/libs{} overrides will be
      //    considered).
      //
      // Because of (2) and (3), this functionality should only be used on a
      // controlled list of libraries (usually libraries that belong to the
      // same family as this library).
      //
      // Note that a similar deduplication is also performed when processing
      // the libraries. However, it may still make sense to do it once at the
      // source for really severe cases (like Boost).
      //
      // Note that this function is not pure.
      //
      f.insert (".deduplicate_export_libs", false).
        insert<const char*, names> (
          [] (const scope* bs,
              vector_view<value> vs,
              const function_overload& f) -> value
          {
            const char* x (*reinterpret_cast<const char* const*> (&f.data));

            if (bs == nullptr)
              fail << f.name << " called out of scope";

            const scope* rs (bs->root_scope ());

            if (rs == nullptr)
              fail << f.name << " called out of project";

            const module* m (rs->find_module<module> (x));

            if (m == nullptr)
              fail << f.name << " called without " << x << " module loaded";

            // We can assume the argument is present due to function's types
            // signature.
            //
            if (vs[0].null)
              throw invalid_argument ("null value");

            names& r (vs[0].as<names> ());
            m->deduplicate_export_libs (*bs,
                                        vector<name> (r.begin (), r.end ()),
                                        r);
            return value (move (r));
          },
          x);

      // $<module>.find_system_library(<name>)
      //
      // Return the library path if the specified library exists in one of the
      // system library search directories. System library search directories
      // are those that the compiler searches by default plus directories
      // specified as part of the compiler mode options (but not *.loptions).
      //
      // The library can be specified in the same form as expected by the
      // linker (-lfoo for POSIX, foo.lib for MSVC) or as a complete name.
      //
      // Note that this function is not pure.
      //
      f.insert (".find_system_library", false).
        insert<const char*, names> (
          [] (const scope* bs,
              vector_view<value> vs,
              const function_overload& f) -> value
          {
            const char* x (*reinterpret_cast<const char* const*> (&f.data));

            if (bs == nullptr)
              fail << f.name << " called out of scope";

            const scope* rs (bs->root_scope ());

            if (rs == nullptr)
              fail << f.name << " called out of project";

            const module* m (rs->find_module<module> (x));

            if (m == nullptr)
              fail << f.name << " called without " << x << " module loaded";

            // We can assume the argument is present due to function's types
            // signature.
            //
            auto r (m->find_system_library (convert<strings> (move (vs[0]))));
            return r ? value (move (*r)) : value (nullptr);
          },
          x);
    }
  }
}
