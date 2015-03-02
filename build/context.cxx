// file      : build/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/context>

#include <ostream>
#include <cassert>

using namespace std;

namespace build
{
  path work;
  path home;

  path src_root;
  path out_root;

  path src_base;
  path out_base;

  path
  src_out (const path& o)
  {
    assert (o.sub (out_root));
    return src_root / o.leaf (out_root);
  }

  path
  out_src (const path& s)
  {
    assert (s.sub (src_root));
    return out_root / s.leaf (src_root);
  }

  path
  relative_work (const path& p)
  {
    if (p.sub (work))
      return p.leaf (work);

    // If work is a sub-path of {src,out}_root and this path is also a
    // sub-bath of it, then use '..' to form a relative path.
    //
    if ((work.sub (src_root) && p.sub (src_root)) ||
        (work.sub (out_root) && p.sub (out_root))) // @@ cache
      return p.relative (work);

    return p;
  }
}
