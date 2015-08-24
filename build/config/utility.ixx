// file      : build/config/utility.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build
{
  namespace config
  {
    template <typename T>
    inline void
    append_options (cstrings& args, T& s, const char* var)
    {
      if (auto l = s[var])
        append_options (args, as<strings> (*l));
    }
  }
}
