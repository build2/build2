// file      : build2/utility.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline size_t
  next_word (const string& s, size_t& b, size_t& e, char d1, char d2)
  {
    return next_word (s, s.size (), b, e, d1, d2);
  }

  inline size_t
  next_word (const string& s, size_t n, size_t& b, size_t& e, char d1, char d2)
  {
    if (b != e)
      b = e;

    // Skip leading delimiters.
    //
    for (; b != n && (s[b] == d1 || s[b] == d2); ++b) ;

    if (b == n)
    {
      e = n;
      return 0;
    }

    // Find first trailing delimiter.
    //
    for (e = b + 1; e != n && s[e] != d1 && s[e] != d2; ++e) ;

    return e - b;
  }

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
