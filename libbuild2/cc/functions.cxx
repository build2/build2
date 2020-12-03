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

namespace build2
{
  const target&
  to_target (const scope&, name&&, name&&); // libbuild2/functions-name.cxx

  namespace cc
  {
    using namespace bin;

    // Common thunk for $x.lib_*(<targets>, <otype> [, ...]) functions.
    //
    struct lib_data
    {
      const char* x;
      void (*f) (void*, strings&,
                 const vector_view<value>&, const module&, const scope&,
                 action, const file&, bool, linfo);
    };

    static value
    lib_thunk_impl (void* ls,
                    const scope* bs,
                    vector_view<value> vs,
                    const function_overload& f)
    {
      const lib_data& d (*reinterpret_cast<const lib_data*> (&f.data));

      if (bs == nullptr)
        fail << f.name << " called out of scope";

      const scope* rs (bs->root_scope ());

      if (rs == nullptr)
        fail << f.name << " called out of project";

      if (bs->ctx.phase != run_phase::execute)
        fail << f.name << " can only be called during execution";

      const module* m (rs->find_module<module> (d.x));

      if (m == nullptr)
        fail << f.name << " called without " << d.x << " module being loaded";

      // We can assume these are present due to function's types signature.
      //
      names& ts_ns (vs[0].as<names> ()); // <targets>
      names& ot_ns (vs[1].as<names> ()); // <otype>

      linfo li;
      {
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

        const file* f;
        bool la (false);

        if ((la = (f = t.is_a<libux> ())) ||
            (la = (f = t.is_a<liba>  ())) ||
            (     (f = t.is_a<libs>  ())))
        {
          d.f (ls, r, vs, *m, *bs, a, *f, la, li);
        }
        else
          fail << t << " is not a library target";
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

    void compile_rule::
    functions (function_family& f, const char* x)
    {
      // $<module>.lib_poptions(<targets>, <otype>)
      //
      // Return the preprocessor options that should be passed when compiling
      // sources that depend on the specified libraries. The second argument
      // is the output target type (obje, objs, etc).
      //
      // Note that passing multiple targets at once is not a mere convenience:
      // this also allows for more effective duplicate suppression.
      //
      // Note also that this function can only be called during execution
      // after all the specified library targets have been matched. Normally
      // it is used in ad hoc recipes to implement custom compilation.
      //
      //
      f[".lib_poptions"].insert<lib_data, names, names> (
        &lib_thunk<appended_libraries>,
        lib_data {
          x,
          [] (void* ls, strings& r,
              const vector_view<value>&, const module& m, const scope& bs,
              action a, const file& l, bool la, linfo li)
          {
            m.append_library_options (
              *static_cast<appended_libraries*> (ls), r, bs, a, l, la, li);
          }});
    }

    void link_rule::
    functions (function_family& f, const char* x)
    {
      // $<module>.lib_libs(<targets>, <otype> [, <flags> [, <self>]])
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
      // Note also that this function can only be called during execution
      // after all the specified library targets have been matched. Normally
      // it is used in ad hoc recipes to implement custom linking.
      //
      f[".lib_libs"].insert<lib_data,
                            names, names, optional<names>, optional<names>> (
        &lib_thunk<appended_libraries>,
        lib_data {
          x,
          [] (void* ls, strings& r,
              const vector_view<value>& vs, const module& m, const scope& bs,
              action a, const file& l, bool la, linfo li)
          {
            lflags lf (0);
            bool rel (true);
            if (vs.size () > 2)
            {
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

            m.append_libraries (*static_cast<appended_libraries*> (ls), r,
                                bs,
                                a, l, la, lf, li, self, rel);
          }});

      // $<module>.lib_rpaths(<targets>, <otype> [, <link> [, <self>]])
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

      // Note also that this function can only be called during execution
      // after all the specified library targets have been matched. Normally
      // it is used in ad hoc recipes to implement custom linking.
      //
      f[".lib_rpaths"].insert<lib_data,
                              names, names, optional<names>, optional<names>> (
        &lib_thunk<rpathed_libraries>,
        lib_data {
          x,
          [] (void* ls, strings& r,
              const vector_view<value>& vs, const module& m, const scope& bs,
              action a, const file& l, bool la, linfo li)
          {
            bool link (vs.size () > 2 ? convert<bool> (vs[2]) : false);
            bool self (vs.size () > 3 ? convert<bool> (vs[3]) : true);
            m.rpath_libraries (*static_cast<rpathed_libraries*> (ls), r,
                               bs,
                               a, l, la, li, link, self);
          }});
    }
  }
}
