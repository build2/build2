// file      : build/prefix-map.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build
{
  template <typename M>
  auto prefix_map_common<M>::
  find_prefix (const key_type& k) -> std::pair<iterator, iterator>
  {
    std::pair<iterator, iterator> r;
    r.first = this->lower_bound (k);

    for (r.second = r.first;
         r.second != this->end ();
         ++r.second)
    {
      if (!this->key_comp ().prefix (k, r.second->first))
        break;
    }

    return r;
  }

  template <typename M>
  auto prefix_map_common<M>::
  find_prefix (const key_type& k) const ->
    std::pair<const_iterator, const_iterator>
  {
    std::pair<const_iterator, const_iterator> r;
    r.first = this->lower_bound (k);

    for (r.second = r.first;
         r.second != this->end ();
         ++r.second)
    {
      if (!this->key_comp ().prefix (k, r.second->first))
        break;
    }

    return r;
  }
}
