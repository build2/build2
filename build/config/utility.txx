// file      : build/config/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/scope>

namespace build
{
  namespace config
  {
    template <typename T>
    std::pair<std::reference_wrapper<const value>, bool>
    required (scope& root, const variable& var, const T& def_value)
    {
      using result = std::pair<std::reference_wrapper<const value>, bool>;

      if (auto l = root[var])
      {
        return l.belongs (*global_scope)
          ? result (root.assign (var) = *l, true)
          : result (*l, false);
      }
      else
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
