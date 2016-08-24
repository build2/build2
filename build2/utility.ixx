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
  append_options (cstrings& args, T& s, const variable& var)
  {
    append_options (args, s[var]);
  }

  template <typename T>
  inline void
  append_options (strings& args, T& s, const variable& var)
  {
    append_options (args, s[var]);
  }

  template <typename T>
  inline void
  append_options (cstrings& args, T& s, const char* var)
  {
    append_options (args, s[var]);
  }

  template <typename T>
  inline void
  append_options (strings& args, T& s, const char* var)
  {
    append_options (args, s[var]);
  }

  template <typename T>
  inline void
  hash_options (sha256& csum, T& s, const variable& var)
  {
    hash_options (csum, s[var]);
  }

  template <typename T>
  inline void
  hash_options (sha256& csum, T& s, const char* var)
  {
    hash_options (csum, s[var]);
  }

  template <typename T>
  inline bool
  find_option (const char* o, T& s, const variable& var, bool ic)
  {
    return find_option (o, s[var], ic);
  }

  template <typename T>
  inline bool
  find_option (const char* o, T& s, const char* var, bool ic)
  {
    return find_option (o, s[var], ic);
  }

  template <typename T>
  inline bool
  find_options (initializer_list<const char*> os,
                T& s,
                const variable& var,
                bool ic)
  {
    return find_options (os, s[var], ic);
  }

  template <typename T>
  inline bool
  find_options (initializer_list<const char*> os,
                T& s,
                const char* var,
                bool ic)
  {
    return find_options (os, s[var], ic);
  }

  template <typename T>
  inline bool
  find_option_prefix (const char* p, T& s, const variable& var, bool ic)
  {
    return find_option_prefix (p, s[var], ic);
  }

  template <typename T>
  inline bool
  find_option_prefix (const char* p, T& s, const char* var, bool ic)
  {
    return find_option_prefix (p, s[var], ic);
  }

  template <typename T>
  inline bool
  find_option_prefixes (initializer_list<const char*> ps,
                        T& s,
                        const variable& var,
                        bool ic)
  {
    return find_option_prefixes (ps, s[var], ic);
  }

  template <typename T>
  inline bool
  find_option_prefixes (initializer_list<const char*> ps,
                        T& s,
                        const char* var,
                        bool ic)
  {
    return find_option_prefixes (ps, s[var], ic);
  }
}
