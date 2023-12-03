// file      : libbuild2/bin/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bin/target.hxx>

#include <libbuild2/context.hxx>

using namespace std;

namespace build2
{
  namespace bin
  {
    const target_type objx::static_type
    {
      "objx",
      &file::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::none
    };

    const target_type bmix::static_type
    {
      "bmix",
      &file::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::none
    };

    const target_type hbmix::static_type
    {
      "hbmix",
      &bmix::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::none
    };

    const target_type libx::static_type
    {
      "libx",
      &mtime_target::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::member_hint // Use untyped hint for group members.
    };

    const target_type libux::static_type
    {
      "libux",
      &file::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::none
    };

    // Note that we link groups during the load phase since this is often
    // relied upon when setting target-specific variables (e.g., we may set a
    // common value for lib{} and then append liba/libs-specific values to
    // it). While sure inelegant, this is MT-safe since during load we are
    // running serial. For the members it is also safe to set the group during
    // creation.

    // obj*{}, lib*{}, and [h]bmi*{} member factory.
    //
    template <typename M, typename G>
    static target*
    m_factory (context& ctx,
               const target_type&, dir_path dir, dir_path out, string n)
    {
      const G* g (ctx.targets.find<G> (dir, out, n));

      M* m (new M (ctx, move (dir), move (out), move (n)));
      m->group = g;

      return m;
    }

    const target_type obje::static_type
    {
      "obje",
      &objx::static_type,
      &m_factory<obje, obj>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type bmie::static_type
    {
      "bmie",
      &bmix::static_type,
      &m_factory<bmie, bmi>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type hbmie::static_type
    {
      "hbmie",
      &hbmix::static_type,
      &m_factory<hbmie, hbmi>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type obja::static_type
    {
      "obja",
      &objx::static_type,
      &m_factory<obja, obj>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type bmia::static_type
    {
      "bmia",
      &bmix::static_type,
      &m_factory<bmia, bmi>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type hbmia::static_type
    {
      "hbmia",
      &hbmix::static_type,
      &m_factory<hbmia, hbmi>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type objs::static_type
    {
      "objs",
      &objx::static_type,
      &m_factory<objs, obj>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type bmis::static_type
    {
      "bmis",
      &bmix::static_type,
      &m_factory<bmis, bmi>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type hbmis::static_type
    {
      "hbmis",
      &hbmix::static_type,
      &m_factory<hbmis, hbmi>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type libue::static_type
    {
      "libue",
      &libux::static_type,
      &target_factory<libue>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type libua::static_type
    {
      "libua",
      &libux::static_type,
      &m_factory<libua, libul>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type libus::static_type
    {
      "libus",
      &libux::static_type,
      &m_factory<libus, libul>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    // obj{}, [h]bmi{}, and libu{} group factory.
    //
    template <typename G, typename E, typename A, typename S>
    static target*
    g_factory (context& ctx,
               const target_type&, dir_path dir, dir_path out, string n)
    {
      // Casts are MT-aware (during serial load).
      //
      E* e (ctx.phase == run_phase::load
            ? const_cast<E*> (ctx.targets.find<E> (dir, out, n))
            : nullptr);
      A* a (ctx.phase == run_phase::load
            ? const_cast<A*> (ctx.targets.find<A> (dir, out, n))
            : nullptr);
      S* s (ctx.phase == run_phase::load
            ? const_cast<S*> (ctx.targets.find<S> (dir, out, n))
            : nullptr);

      G* g (new G (ctx, move (dir), move (out), move (n)));

      if (e != nullptr) e->group = g;
      if (a != nullptr) a->group = g;
      if (s != nullptr) s->group = g;

      return g;
    }

    const target_type obj::static_type
    {
      "obj",
      &target::static_type,
      &g_factory<obj, obje, obja, objs>,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::member_hint // Use untyped hint for group members.
    };

    const target_type bmi::static_type
    {
      "bmi",
      &target::static_type,
      &g_factory<bmi, bmie, bmia, bmis>,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::member_hint // Use untyped hint for group members.
    };

    const target_type hbmi::static_type
    {
      "hbmi",
      &target::static_type,
      &g_factory<hbmi, hbmie, hbmia, hbmis>,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::member_hint // Use untyped hint for group members.
    };

    // The same as g_factory() but without E.
    //
    static target*
    libul_factory (context& ctx,
                   const target_type&, dir_path dir, dir_path out, string n)
    {
      libua* a (ctx.phase == run_phase::load
                ? const_cast<libua*> (ctx.targets.find<libua> (dir, out, n))
                : nullptr);
      libus* s (ctx.phase == run_phase::load
                ? const_cast<libus*> (ctx.targets.find<libus> (dir, out, n))
                : nullptr);

      libul* g (new libul (ctx, move (dir), move (out), move (n)));

      if (a != nullptr) a->group = g;
      if (s != nullptr) s->group = g;

      return g;
    }

    const target_type libul::static_type
    {
      "libul",
      &libx::static_type,
      &libul_factory,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::member_hint // Use untyped hint for group members.
    };

    // What extensions should we use? At the outset, this is platform-
    // dependent. And if we consider cross-compilation, is it build or
    // host-dependent? Feels like it should be host-dependent so that
    // we can copy things between cross and native environments. So
    // these will have to be determined based on what we are building.
    // As if this is not complicated enough, the bin module doesn't
    // know anything about building. So perhaps the extension should
    // come from a variable that is set not by bin but by the module
    // whose rule matched the target (e.g., cxx::link).
    //
    const target_type liba::static_type
    {
      "liba",
      &file::static_type,
      &m_factory<liba, lib>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    const target_type libs::static_type
    {
      "libs",
      &file::static_type,
      &m_factory<libs, lib>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    // lib
    //
    group_view lib::
    group_members (action) const
    {
      static_assert (sizeof (lib_members) == sizeof (const target*) * 2,
                     "member layout incompatible with array");

      return a != nullptr || s != nullptr
        ? group_view {reinterpret_cast<const target* const*> (&a), 2}
        : group_view {nullptr, 0};
    }

    static target*
    lib_factory (context& ctx,
                 const target_type&, dir_path dir, dir_path out, string n)
    {
      // Casts are MT-aware (during serial load).
      //
      liba* a (ctx.phase == run_phase::load
               ? const_cast<liba*> (ctx.targets.find<liba> (dir, out, n))
               : nullptr);
      libs* s (ctx.phase == run_phase::load
               ? const_cast<libs*> (ctx.targets.find<libs> (dir, out, n))
               : nullptr);

      lib* l (new lib (ctx, move (dir), move (out), move (n)));

      if (a != nullptr) a->group = l;
      if (s != nullptr) s->group = l;

      return l;
    }

    const target_type lib::static_type
    {
      "lib",
      &libx::static_type,
      &lib_factory,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,

      // Note: not see-through ("alternatives" group).
      //
      target_type::flag::member_hint // Use untyped hint for group members.
    };

    // libi
    //
    const target_type libi::static_type
    {
      "libi",
      &file::static_type,
      &target_factory<libi>,
      nullptr, /* fixed_extension */
      &target_extension_var<nullptr>,
      &target_pattern_var<nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      target_type::flag::none
    };

    // def
    //
    extern const char def_ext[] = "def"; // VC14 rejects constexpr.

    const target_type def::static_type
    {
      "def",
      &file::static_type,
      &target_factory<def>,
      &target_extension_fix<def_ext>,
      nullptr,                          /* default_extension */
      &target_pattern_fix<def_ext>,
      nullptr,
      &file_search,
      target_type::flag::none
    };
  }
}
