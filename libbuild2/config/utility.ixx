// file      : libbuild2/config/utility.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace config
  {
    LIBBUILD2_SYMEXPORT pair<lookup, bool>
    lookup_config_impl (scope&, const variable&, uint64_t);

    template <typename T>
    pair<lookup, bool>
    lookup_config_impl (scope&, const variable&, T&&, uint64_t, bool);

    inline lookup
    lookup_config (scope& rs, const variable& var, uint64_t sflags)
    {
      return lookup_config_impl (rs, var, sflags).first;
    }

    inline lookup
    lookup_config (bool& new_value,
                   scope& rs,
                   const variable& var,
                   uint64_t sflags)
    {
      auto r (lookup_config_impl (rs, var, sflags));
      new_value = new_value || r.second;
      return r.first;
    }

    template <typename T>
    inline lookup
    lookup_config (scope& rs,
                   const variable& var,
                   T&& def_val,
                   uint64_t sflags,
                   bool def_ovr)
    {
      return lookup_config_impl (rs,
                                 var,
                                 std::forward<T> (def_val), // VC14
                                 sflags,
                                 def_ovr).first;
    }

    template <typename T>
    inline lookup
    lookup_config (bool& new_value,
                   scope& rs,
                   const variable& var,
                   T&& def_val,
                   uint64_t sflags,
                   bool def_ovr)
    {
      auto r (lookup_config_impl (rs,
                                  var,
                                  std::forward<T> (def_val), // VC14
                                  sflags,
                                  def_ovr));
      new_value = new_value || r.second;
      return r.first;
    }
  }
}
