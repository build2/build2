// file      : build/bin/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/bin/target>

using namespace std;

namespace build
{
  namespace bin
  {
    static target*
    obja_factory (dir_path d, std::string n, const std::string* e)
    {
      obj* o (targets.find<obj> (d, n));
      obja* a (new obja (std::move (d), std::move (n), e));

      if ((a->group = o))
        o->a = a;

      return a;
    }

    const target_type obja::static_type
    {
      typeid (obja),
      "obja",
      &file::static_type,
      &obja_factory,
      nullptr,
      &search_target, // Note: not _file(); don't look for an existing file.
      false
    };

    static target*
    objso_factory (dir_path d, std::string n, const std::string* e)
    {
      obj* o (targets.find<obj> (d, n));
      objso* so (new objso (std::move (d), std::move (n), e));

      if ((so->group = o))
        o->so = so;

      return so;
    }

    const target_type objso::static_type
    {
      typeid (objso),
      "objso",
      &file::static_type,
      &objso_factory,
      nullptr,
      &search_target, // Note: not _file(); don't look for an existing file.
      false
    };

    static target*
    obj_factory (dir_path d, string n, const string* e)
    {
      obja* a (targets.find<obja> (d, n));
      objso* so (targets.find<objso> (d, n));
      obj* o (new obj (move (d), move (n), e));

      if ((o->a = a))
        a->group = o;

      if ((o->so = so))
        so->group = o;

      return o;
    }

    const target_type obj::static_type
    {
      typeid (obj),
      "obj",
      &target::static_type,
      &obj_factory,
      nullptr,
      &search_target,
      false
    };

    const target_type exe::static_type
    {
      typeid (exe),
      "exe",
      &file::static_type,
      &target_factory<exe>,
      nullptr,
      &search_file,
      false
    };

    static target*
    liba_factory (dir_path d, std::string n, const std::string* e)
    {
      lib* l (targets.find<lib> (d, n));
      liba* a (new liba (std::move (d), std::move (n), e));

      if ((a->group = l))
        l->a = a;

      return a;
    }

    // @@
    //
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
    constexpr const char a_ext[] = "a";
    const target_type liba::static_type
    {
      typeid (liba),
      "liba",
      &file::static_type,
      &liba_factory,
      &target_extension_fix<a_ext>,
      &search_file,
      false
    };

    static target*
    libso_factory (dir_path d, std::string n, const std::string* e)
    {
      lib* l (targets.find<lib> (d, n));
      libso* so (new libso (std::move (d), std::move (n), e));

      if ((so->group = l))
        l->so = so;

      return so;
    }

    constexpr const char so_ext[] = "so";
    const target_type libso::static_type
    {
      typeid (libso),
      "libso",
      &file::static_type,
      &libso_factory,
      &target_extension_fix<so_ext>,
      &search_file,
      false
    };

    // lib
    //
    void lib::
    reset (action_type)
    {
      // Don't clear prerequisite_targets since it is "given" to our
      // members to implement "library meta-information protocol".
    }

    static target*
    lib_factory (dir_path d, string n, const string* e)
    {
      liba* a (targets.find<liba> (d, n));
      libso* so (targets.find<libso> (d, n));
      lib* l (new lib (move (d), move (n), e));

      if ((l->a = a))
        a->group = l;

      if ((l->so = so))
        so->group = l;

      return l;
    }

    const target_type lib::static_type
    {
      typeid (lib),
      "lib",
      &target::static_type,
      &lib_factory,
      nullptr,
      &search_target,
      false
    };
  }
}
