// file      : libbuild2/file.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/export.hxx>

namespace build2
{
  inline bool
  source_once (scope& root, scope& base, const path& bf)
  {
    return source_once (root, base, bf, base);
  }

  LIBBUILD2_SYMEXPORT const target*
  import (const prerequisite_key&, bool existing);

  inline const target&
  import (const prerequisite_key& pk)
  {
    assert (phase == run_phase::match);
    return *import (pk, false);
  }

  inline const target*
  import_existing (const prerequisite_key& pk)
  {
    assert (phase == run_phase::match || phase == run_phase::execute);
    return import (pk, true);
  }
}
