// file      : build2/cc/utility.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace cc
  {
    inline otype
    compile_type (const target& t, bool mod)
    {
      using namespace bin;

      return
        t.is_a (mod ? bmie::static_type : obje::static_type) ? otype::e :
        t.is_a (mod ? bmia::static_type : obja::static_type) ? otype::a :
        otype::s;
    }

    inline otype
    link_type (const target& t)
    {
      using namespace bin;

      return
        t.is_a<exe> ()  ? otype::e :
        t.is_a<liba> () ? otype::a :
        otype::s;
    }

    inline compile_target_types
    compile_types (otype t)
    {
      using namespace bin;

      const target_type* o (nullptr);
      const target_type* m (nullptr);

      switch (t)
      {
      case otype::e: o = &obje::static_type; m = &bmie::static_type; break;
      case otype::a: o = &obja::static_type; m = &bmia::static_type; break;
      case otype::s: o = &objs::static_type; m = &bmis::static_type; break;
      }

      return compile_target_types {*o, *m};
    }
  }
}
