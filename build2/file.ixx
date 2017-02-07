// file      : build2/file.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline bool
  source_once (scope& root, scope& base, const path& bf)
  {
    return source_once (root, base, bf, base);
  }

  target*
  import (const prerequisite_key&, bool existing);

  inline target&
  import (const prerequisite_key& pk)
  {
    assert (phase == run_phase::search_match);
    return *import (pk, false);
  }

  inline const target*
  import_existing (const prerequisite_key& pk)
  {
    assert (phase == run_phase::search_match || phase == run_phase::execute);
    return import (pk, true);
  }
}
