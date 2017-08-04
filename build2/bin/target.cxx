// file      : build2/bin/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/target.hxx>

using namespace std;

namespace build2
{
  namespace bin
  {
    const target_type libx::static_type
    {
      "libx",
      &mtime_target::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      false
    };

    const target_type libux::static_type
    {
      "libux",
      &file::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      false
    };

    // Note that we link groups during the load phase since this is often
    // relied upon when setting target-specific variables (e.g., we may set a
    // common value for lib{} and then append liba/libs-specific values to
    // it). While sure inelegant, this is MT-safe since during load we are
    // running serial. For the members it is also safe to set the group during
    // creation.

    extern const char ext_var[] = "extension"; // VC14 rejects constexpr.

    // obj*{}, bmi*{}, libu*{} member factory.
    //
    template <typename M, typename G>
    static pair<target*, optional<string>>
    m_factory (const target_type&,
               dir_path dir,
               dir_path out,
               string n,
               optional<string> ext)
    {
      const G* g (targets.find<G> (dir, out, n));

      M* m (new M (move (dir), move (out), move (n)));
      m->group = g;

      return make_pair (m, move (ext));
    }

    const target_type obje::static_type
    {
      "obje",
      &file::static_type,
      &m_factory<obje, obj>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    const target_type bmie::static_type
    {
      "bmie",
      &file::static_type,
      &m_factory<bmie, bmi>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    const target_type libue::static_type
    {
      "libue",
      &libux::static_type,
      &m_factory<libue, libu>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    const target_type obja::static_type
    {
      "obja",
      &file::static_type,
      &m_factory<obja, obj>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    const target_type bmia::static_type
    {
      "bmia",
      &file::static_type,
      &m_factory<bmia, bmi>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    const target_type libua::static_type
    {
      "libua",
      &libux::static_type,
      &m_factory<libua, libu>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    const target_type objs::static_type
    {
      "objs",
      &file::static_type,
      &m_factory<objs, obj>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    const target_type bmis::static_type
    {
      "bmis",
      &file::static_type,
      &m_factory<bmis, bmi>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    const target_type libus::static_type
    {
      "libus",
      &libux::static_type,
      &m_factory<libus, libu>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    // obj{}, bmi{}, and libu{} group factory.
    //
    template <typename G, typename E, typename A, typename S>
    static pair<target*, optional<string>>
    g_factory (const target_type&,
               dir_path dir,
               dir_path out,
               string n,
               optional<string> ext)
    {
      // Casts are MT-aware (during serial load).
      //
      E* e (phase == run_phase::load
            ? const_cast<E*> (targets.find<E> (dir, out, n))
            : nullptr);
      A* a (phase == run_phase::load
            ? const_cast<A*> (targets.find<A> (dir, out, n))
            : nullptr);
      S* s (phase == run_phase::load
            ? const_cast<S*> (targets.find<S> (dir, out, n))
            : nullptr);

      G* g (new G (move (dir), move (out), move (n)));

      if (e != nullptr) e->group = g;
      if (a != nullptr) a->group = g;
      if (s != nullptr) s->group = g;

      return make_pair (g, move (ext));
    }

    const target_type obj::static_type
    {
      "obj",
      &target::static_type,
      &g_factory<obj, obje, obja, objs>,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      false
    };

    const target_type bmi::static_type
    {
      "bmi",
      &target::static_type,
      &g_factory<bmi, bmie, bmia, bmis>,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      false
    };

    const target_type libu::static_type
    {
      "libu",
      &libx::static_type,
      &g_factory<libu, libue, libua, libus>,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      false
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
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &file_search,
      false
    };

    const target_type libs::static_type
    {
      "libs",
      &file::static_type,
      &m_factory<libs, lib>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &file_search,
      false
    };

    // lib
    //
    group_view lib::
    group_members (action_type) const
    {
      static_assert (sizeof (lib_members) == sizeof (const target*) * 2,
                     "member layout incompatible with array");

      return a != nullptr || s != nullptr
        ? group_view {reinterpret_cast<const target* const*> (&a), 2}
        : group_view {nullptr, 0};
    }

    static pair<target*, optional<string>>
    lib_factory (const target_type&,
                 dir_path dir,
                 dir_path out,
                 string n,
                 optional<string> ext)
    {
      // Casts are MT-aware (during serial load).
      //
      liba* a (phase == run_phase::load
               ? const_cast<liba*> (targets.find<liba> (dir, out, n))
               : nullptr);
      libs* s (phase == run_phase::load
               ? const_cast<libs*> (targets.find<libs> (dir, out, n))
               : nullptr);

      lib* l (new lib (move (dir), move (out), move (n)));

      if (a != nullptr) a->group = l;
      if (s != nullptr) s->group = l;

      return make_pair (l, move (ext));
    }

    const target_type lib::static_type
    {
      "lib",
      &libx::static_type,
      &lib_factory,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      false
    };

    // libi
    //
    const target_type libi::static_type
    {
      "libi",
      &file::static_type,
      &target_factory<libi>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &file_search,
      false
    };
  }
}
