// file      : build/string-table.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <limits>  // numeric_limits
#include <cstddef> // size_t
#include <cassert>

namespace build
{
  template <typename I, typename D>
  I string_table<I, D>::
  insert (const D& d)
  {
    std::size_t i (vec_.size () + 1);

    // Note: move(d) would be tricky since key still points to it.
    //
    auto r (map_.emplace (
              key_type (&traits::key (d)),
              value_type {static_cast<I> (i), d}));

    if (r.second)
    {
      assert (i <= std::numeric_limits<I>::max ());

      r.first->first.p = &traits::key (r.first->second.d); // Update key.
      vec_.push_back (r.first);
    }

    return r.first->second.i;
  }
}
