// file      : build2/file.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline void
  source_once (const path& bf, scope& root, scope& base)
  {
    return source_once (bf, root, base, base);
  }
}
