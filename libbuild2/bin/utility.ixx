// file      : libbuild2/bin/utility.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace bin
  {
    inline ltype
    link_type (const target_type& tt)
    {
      bool u (false);
      otype o (
        tt.is_a<exe>  () || (u = tt.is_a<libue> ()) ? otype::e :
        tt.is_a<liba> () || (u = tt.is_a<libua> ()) ? otype::a :
        tt.is_a<libs> () || (u = tt.is_a<libus> ()) ? otype::s :
        static_cast<otype> (0xFF));

      return ltype {o, u};
    }

    inline pair<otype, bool>
    link_member (lmembers lm, lorder lo)
    {
      bool r (true);

      bool s (true);
      switch (lo)
      {
      case lorder::a:
      case lorder::a_s:
        s = false; // Fall through.
      case lorder::s:
      case lorder::s_a:
        {
          if (s ? !lm.s : !lm.a)
          {
            if (lo == lorder::a_s || lo == lorder::s_a)
              s = !s;
            else
              r = false; // Not available.
          }
        }
      }

      return make_pair (s ? otype::s : otype::a, r);
    }
  }
}
