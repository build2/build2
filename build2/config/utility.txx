// file      : build2/config/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scope>

namespace build2
{
  namespace config
  {
    template <typename T>
    std::pair<std::reference_wrapper<const value>, bool>
    required (scope& root, const variable& var, const T& def_value, bool ovr)
    {
      using result = std::pair<std::reference_wrapper<const value>, bool>;

      if (auto l = root[var])
      {
        if (l.belongs (*global_scope))
          return result (root.assign (var) = *l, true);

        if (!ovr || l.belongs (root))
          return result (*l, false);
      }

      return result (root.assign (var) = def_value, true);
    }

    template <typename T>
    bool
    find_option (const char* option, T& s, const char* var)
    {
      if (auto l = s[var])
      {
        for (const std::string& s: as<strings> (*l))
        {
          if (s == option)
            return true;
        }
      }

      return false;
    }
  }
}
