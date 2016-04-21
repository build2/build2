// file      : build2/bin/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/target>

using namespace std;

namespace build2
{
  namespace bin
  {
    constexpr const char ext_var[] = "extension";

    static target*
    obja_factory (const target_type&,
                  dir_path dir,
                  dir_path out,
                  string n,
                  const string* e)
    {
      obj* o (targets.find<obj> (dir, out, n));
      obja* a (new obja (move (dir), move (out), move (n), e));

      if ((a->group = o))
        o->a = a;

      return a;
    }

    const target_type obja::static_type
    {
      "obja",
      &file::static_type,
      &obja_factory,
      &target_extension_var<ext_var, nullptr>,
      nullptr,
      &search_target, // Note: not _file(); don't look for an existing file.
      false
    };

    static target*
    objso_factory (const target_type&,
                   dir_path dir,
                   dir_path out,
                   string n,
                   const string* e)
    {
      obj* o (targets.find<obj> (dir, out, n));
      objso* so (new objso (move (dir), move (out), move (n), e));

      if ((so->group = o))
        o->so = so;

      return so;
    }

    const target_type objso::static_type
    {
      "objso",
      &file::static_type,
      &objso_factory,
      &target_extension_var<ext_var, nullptr>,
      nullptr,
      &search_target, // Note: not _file(); don't look for an existing file.
      false
    };

    static target*
    obj_factory (const target_type&,
                 dir_path dir,
                 dir_path out,
                 string n,
                 const string* e)
    {
      obja* a (targets.find<obja> (dir, out, n));
      objso* so (targets.find<objso> (dir, out, n));
      obj* o (new obj (move (dir), move (out), move (n), e));

      if ((o->a = a))
        a->group = o;

      if ((o->so = so))
        so->group = o;

      return o;
    }

    const target_type obj::static_type
    {
      "obj",
      &target::static_type,
      &obj_factory,
      nullptr,
      nullptr,
      &search_target,
      false
    };

    // @@ What extension should we be using when searching for an existing
    //    exe{}? Say we have a dependency on some pre-existing tool, maybe
    //    some source code generator. Should we use 'build' extension? But
    //    what if we find such an executable for something that we need to
    //    build for 'host'?
    //
    //    What if we use extension variables and scoping. We could set the
    //    root scope exe{*} extension to 'build' and then, say, cxx module
    //    (or any module that knows how to build exe{}) changes it to the
    //    'host'. Maybe that's not a bad idea?
    //
    const target_type exe::static_type
    {
      "exe",
      &file::static_type,
      &target_factory<exe>,
      &target_extension_var<ext_var, nullptr>,
      nullptr,
      &search_file,
      false
    };

    static target*
    liba_factory (const target_type& t,
                  dir_path d,
                  dir_path o,
                  string n,
                  const string* e)
    {
      // Only link-up to the group if the types match exactly.
      //
      lib* l (t == liba::static_type ? targets.find<lib> (d, o, n) : nullptr);
      liba* a (new liba (move (d), move (o), move (n), e));

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
    const target_type liba::static_type
    {
      "liba",
      &file::static_type,
      &liba_factory,
      &target_extension_var<ext_var, nullptr>,
      nullptr,
      &search_file,
      false
    };

    static target*
    libso_factory (const target_type& t,
                   dir_path d,
                   dir_path o,
                   string n,
                   const string* e)
    {
      // Only link-up to the group if the types match exactly.
      //
      lib* l (t == libso::static_type ? targets.find<lib> (d, o, n) : nullptr);
      libso* so (new libso (move (d), move (o), move (n), e));

      if ((so->group = l))
        l->so = so;

      return so;
    }

    const target_type libso::static_type
    {
      "libso",
      &file::static_type,
      &libso_factory,
      &target_extension_var<ext_var, nullptr>,
      nullptr,
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
      //
      raw_state = target_state::unknown;
    }

    static target*
    lib_factory (const target_type&,
                 dir_path d,
                 dir_path o,
                 string n,
                 const string* e)
    {
      liba* a (targets.find<liba> (d, o, n));
      libso* so (targets.find<libso> (d, o, n));
      lib* l (new lib (move (d), move (o), move (n), e));

      if ((l->a = a))
        a->group = l;

      if ((l->so = so))
        so->group = l;

      return l;
    }

    const target_type lib::static_type
    {
      "lib",
      &target::static_type,
      &lib_factory,
      nullptr,
      nullptr,
      &search_target,
      false
    };
  }
}
