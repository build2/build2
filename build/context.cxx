// file      : build/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/context>

#include <ostream>
#include <cassert>

#include <build/scope>

using namespace std;

namespace build
{
  path work;
  path home;

  path
  src_out (const path& out, scope& s)
  {
    return src_out (out,
                    s["out_root"].as<const path&> (),
                    s["src_root"].as<const path&> ());
  }

  path
  out_src (const path& src, scope& s)
  {
    return out_src (src,
                    s["out_root"].as<const path&> (),
                    s["src_root"].as<const path&> ());
  }

  path
  src_out (const path& o, const path& out_root, const path& src_root)
  {
    assert (o.sub (out_root));
    return src_root / o.leaf (out_root);
  }

  path
  out_src (const path& s, const path& out_root, const path& src_root)
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
    // sub-path of it, then use '..' to form a relative path.
    //
    // Don't think this is a good heuristic. For example, why shouldn't
    // we display paths from imported projects as relative if they are
    // more readable than absolute?
    //
    /*
    if ((work.sub (src_root) && p.sub (src_root)) ||
        (work.sub (out_root) && p.sub (out_root)))
      return p.relative (work);
    */

    if (p.root_directory () == work.root_directory ())
    {
      path r (p.relative (work));

      if (r.string ().size () < p.string ().size ())
        return r;
    }

    return p;
  }
}
