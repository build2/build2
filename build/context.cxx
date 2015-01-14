// file      : build/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/context>

#include <ostream>

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
  translate (const path& p)
  {
    if (p.sub (work))
      return p.leaf (work);

    // If work is a sub-path of {src,out}_root and this path is also a
    // sub-bath of it, then use '..' to form a relative path.
    //
    if (work.sub (src_root) && p.sub (src_root) ||
        work.sub (out_root) && p.sub (out_root)) // @@ cache
      return p.relative (work);

    return p;
  }

  std::string
  diagnostic_string (const path& p)
  {
    if (p.absolute ())
    {
      path rp (translate (p));

#ifndef _WIN32
      if (rp.absolute () && rp.sub (home))
        return "~/" + rp.leaf (home).string ();
#endif

      return rp.string ();
    }

    return p.string ();
  }
}
