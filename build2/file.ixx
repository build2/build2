// file      : build2/file.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline bool
  source_once (scope& root, scope& base, const path& bf)
  {
    return source_once (root, base, bf, base);
  }

  const target*
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

  void
  source_hooks (scope&, const dir_path&, bool);

  inline void
  bootstrap_pre (scope& root)
  {
    source_hooks (root, bootstrap_dir, true /* pre */);
  }

  inline void
  bootstrap_post (scope& root)
  {
    source_hooks (root, bootstrap_dir, false /* pre */);
  }
}
