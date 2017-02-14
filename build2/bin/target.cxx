// file      : build2/bin/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/target>

using namespace std;

namespace build2
{
  namespace bin
  {
    extern const char ext_var[] = "extension"; // VC14 rejects constexpr.

    static pair<target*, optional<string>>
    obje_factory (const target_type&,
                  dir_path dir,
                  dir_path out,
                  string n,
                  optional<string> ext)
    {
      obj* o (targets.find<obj> (dir, out, n));
      obje* e (new obje (move (dir), move (out), move (n)));

      if ((e->group = o) != nullptr)
        o->e = e;

      return make_pair (e, move (ext));
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

    static pair<target*, optional<string>>
    obja_factory (const target_type&,
                  dir_path dir,
                  dir_path out,
                  string n,
                  optional<string> ext)
    {
      obj* o (targets.find<obj> (dir, out, n));
      obja* a (new obja (move (dir), move (out), move (n)));

      if ((a->group = o) != nullptr)
        o->a = a;

      return make_pair (a, move (ext));
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

    static pair<target*, optional<string>>
    objs_factory (const target_type&,
                   dir_path dir,
                   dir_path out,
                   string n,
                   optional<string> ext)
    {
      obj* o (targets.find<obj> (dir, out, n));
      objs* s (new objs (move (dir), move (out), move (n)));

      if ((s->group = o) != nullptr)
        o->s = s;

      return make_pair (s, move (ext));
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

    static pair<target*, optional<string>>
    obj_factory (const target_type&,
                 dir_path dir,
                 dir_path out,
                 string n,
                 optional<string> ext)
    {
      obje* e (targets.find<obje> (dir, out, n));
      obja* a (targets.find<obja> (dir, out, n));
      objs* s (targets.find<objs> (dir, out, n));

      obj* o (new obj (move (dir), move (out), move (n)));

      if ((o->e = e) != nullptr)
        e->group = o;

      if ((o->a = a)!= nullptr)
        a->group = o;

      if ((o->s = s)!= nullptr)
        s->group = o;

      return make_pair (o, move (ext));
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

    static pair<target*, optional<string>>
    liba_factory (const target_type& t,
                  dir_path d,
                  dir_path o,
                  string n,
                  optional<string> ext)
    {
      // Only link-up to the group if the types match exactly.
      //
      lib* l (t == liba::static_type ? targets.find<lib> (d, o, n) : nullptr);
      liba* a (new liba (move (d), move (o), move (n)));

      if ((a->group = l) != nullptr)
        l->a = a;

      return make_pair (a, move (ext));
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

    static pair<target*, optional<string>>
    libs_factory (const target_type& t,
                  dir_path d,
                  dir_path o,
                  string n,
                  optional<string> ext)
    {
      // Only link-up to the group if the types match exactly.
      //
      lib* l (t == libs::static_type ? targets.find<lib> (d, o, n) : nullptr);
      libs* s (new libs (move (d), move (o), move (n)));

      if ((s->group = l) != nullptr)
        l->s = s;

      return make_pair (s, move (ext));
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
    static pair<target*, optional<string>>
    lib_factory (const target_type&,
                 dir_path d,
                 dir_path o,
                 string n,
                 optional<string> ext)
    {
      liba* a (targets.find<liba> (d, o, n));
      libs* s (targets.find<libs> (d, o, n));

      lib* l (new lib (move (d), move (o), move (n)));

      if ((l->a = a) != nullptr)
        a->group = l;

      if ((l->s = s) != nullptr)
        s->group = l;

      return make_pair (l, move (ext));
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
