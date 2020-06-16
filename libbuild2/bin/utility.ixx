// file      : libbuild2/bin/utility.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace bin
  {
    inline ltype
    link_type (const target& t)
    {
      bool u (false);
      otype o (
        t.is_a<exe>  () || (u = t.is_a<libue> ()) ? otype::e :
        t.is_a<liba> () || (u = t.is_a<libua> ()) ? otype::a :
        t.is_a<libs> () || (u = t.is_a<libus> ()) ? otype::s :
        static_cast<otype> (0xFF));

      return ltype {o, u};
    }
  }
}
