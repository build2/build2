// file      : build2/bin/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/target>

using namespace std;

namespace build2
{
  namespace bin
  {
    extern const char ext_var[] = "extension"; // VC 19 rejects constexpr.

    static target*
    obje_factory (const target_type&,
                  dir_path dir,
                  dir_path out,
                  string n,
                  const string* ext)
    {
      obj* o (targets.find<obj> (dir, out, n));
      obje* e (new obje (move (dir), move (out), move (n), ext));

      if ((e->group = o))
        o->e = e;

      return e;
    }

    const target_type obje::static_type
    {
      "obje",
      &file::static_type,
      &obje_factory,
      &target_extension_var<ext_var, nullptr>,
      nullptr,
      &search_target, // Note: not _file(); don't look for an existing file.
      false
    };

    static target*
    obja_factory (const target_type&,
                  dir_path dir,
                  dir_path out,
                  string n,
                  const string* ext)
    {
      obj* o (targets.find<obj> (dir, out, n));
      obja* a (new obja (move (dir), move (out), move (n), ext));

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
    objs_factory (const target_type&,
                   dir_path dir,
                   dir_path out,
                   string n,
                   const string* ext)
    {
      obj* o (targets.find<obj> (dir, out, n));
      objs* s (new objs (move (dir), move (out), move (n), ext));

      if ((s->group = o))
        o->s = s;

      return s;
    }

    const target_type objs::static_type
    {
      "objs",
      &file::static_type,
      &objs_factory,
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
                 const string* ext)
    {
      obje* e (targets.find<obje> (dir, out, n));
      obja* a (targets.find<obja> (dir, out, n));
      objs* s (targets.find<objs> (dir, out, n));

      obj* o (new obj (move (dir), move (out), move (n), ext));

      if ((o->e = e))
        e->group = o;

      if ((o->a = a))
        a->group = o;

      if ((o->s = s))
        s->group = o;

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
                  const string* ext)
    {
      // Only link-up to the group if the types match exactly.
      //
      lib* l (t == liba::static_type ? targets.find<lib> (d, o, n) : nullptr);
      liba* a (new liba (move (d), move (o), move (n), ext));

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
    libs_factory (const target_type& t,
                  dir_path d,
                  dir_path o,
                  string n,
                  const string* ext)
    {
      // Only link-up to the group if the types match exactly.
      //
      lib* l (t == libs::static_type ? targets.find<lib> (d, o, n) : nullptr);
      libs* s (new libs (move (d), move (o), move (n), ext));

      if ((s->group = l))
        l->s = s;

      return s;
    }

    const target_type libs::static_type
    {
      "libs",
      &file::static_type,
      &libs_factory,
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
    }

    static target*
    lib_factory (const target_type&,
                 dir_path d,
                 dir_path o,
                 string n,
                 const string* ext)
    {
      liba* a (targets.find<liba> (d, o, n));
      libs* s (targets.find<libs> (d, o, n));

      lib* l (new lib (move (d), move (o), move (n), ext));

      if ((l->a = a))
        a->group = l;

      if ((l->s = s))
        s->group = l;

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

    // libi
    //
    const target_type libi::static_type
    {
      "libi",
      &file::static_type,
      &target_factory<libi>,
      &target_extension_var<ext_var, nullptr>,
      nullptr,
      &search_file,
      false
    };
  }
}
