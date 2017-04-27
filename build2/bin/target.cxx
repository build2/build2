// file      : build2/bin/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/target>

using namespace std;

namespace build2
{
  namespace bin
  {
    // Note that we link groups during the load phase since this is often
    // relied upon when setting target-specific variables (e.g., we may set a
    // common value for lib{} and then append liba/libs-specific values to
    // it). While sure inelegant, this is MT-safe since during load we are
    // running serial. For the members it is also safe to set the group during
    // creation.

    extern const char ext_var[] = "extension"; // VC14 rejects constexpr.

    template <typename T>
    static pair<target*, optional<string>>
    objx_factory (const target_type&,
                  dir_path dir,
                  dir_path out,
                  string n,
                  optional<string> ext)
    {
      const obj* g (targets.find<obj> (dir, out, n));

      T* x (new T (move (dir), move (out), move (n)));
      x->group = g;

      return make_pair (x, move (ext));
    }

    const target_type obje::static_type
    {
      "obje",
      &file::static_type,
      &objx_factory<obje>,
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
      &objx_factory<obja>,
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
      &objx_factory<objs>,
      &target_extension_var<ext_var, nullptr>,
      &target_pattern_var<ext_var, nullptr>,
      nullptr,
      &target_search, // Note: not _file(); don't look for an existing file.
      false
    };

    static pair<target*, optional<string>>
    obj_factory (const target_type&,
                 dir_path dir,
                 dir_path out,
                 string n,
                 optional<string> ext)
    {
      // Casts are MT-aware (during serial load).
      //
      obje* e (phase == run_phase::load
               ? const_cast<obje*> (targets.find<obje> (dir, out, n))
               : nullptr);
      obja* a (phase == run_phase::load
               ? const_cast<obja*> (targets.find<obja> (dir, out, n))
               : nullptr);
      objs* s (phase == run_phase::load
               ? const_cast<objs*> (targets.find<objs> (dir, out, n))
               : nullptr);

      obj* o (new obj (move (dir), move (out), move (n)));

      if (e != nullptr) e->group = o;
      if (a != nullptr) a->group = o;
      if (s != nullptr) s->group = o;

      return make_pair (o, move (ext));
    }

    const target_type obj::static_type
    {
      "obj",
      &target::static_type,
      &obj_factory,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      false
    };

    template <typename T>
    static pair<target*, optional<string>>
    libx_factory (const target_type&,
                  dir_path dir,
                  dir_path out,
                  string n,
                  optional<string> ext)
    {
      const lib* g (targets.find<lib> (dir, out, n));

      T* x (new T (move (dir), move (out), move (n)));
      x->group = g;

      return make_pair (x, move (ext));
    }

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
      &libx_factory<liba>,
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
      &libx_factory<libs>,
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
      &target::static_type,
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
