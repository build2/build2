// file      : build/file.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  inline void
  source_once (const path& bf, scope& root, scope& base)
  {
    return source_once (bf, root, base, base);
  }
}
