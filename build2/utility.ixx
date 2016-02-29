// file      : build2/utility.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  template <typename T>
  inline void
  append_options (cstrings& args, T& s, const char* var)
  {
    append_options (args, s[var]);
  }

  template <typename T>
  inline void
  hash_options (sha256& csum, T& s, const char* var)
  {
    hash_options (csum, s[var]);
  }

  template <typename T>
  inline bool
  find_option (const char* option, T& s, const char* var)
  {
    return find_option (option, s[var]);
  }
}
