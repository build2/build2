// file      : build/prefix_map.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  template <typename M>
  auto prefix_map_impl<M>::
  find (const key_type& k) -> std::pair<iterator, iterator>
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
  auto prefix_map_impl<M>::
  find (const key_type& k) const -> std::pair<const_iterator, const_iterator>
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
