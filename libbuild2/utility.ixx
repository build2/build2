// file      : libbuild2/utility.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cstring> // strlen() strchr()

namespace build2
{
  inline void
  hash_path (sha256& cs, const path& p, const dir_path& prefix)
  {
    // Note: for efficiency we don't use path::leaf() and "skip" the prefix
    // without copying.
    //
    const char* s (p.string ().c_str ());

    if (!prefix.empty () && p.sub (prefix))
    {
      s += prefix.size (); // Does not include trailing slash except for root.
      if (path::traits_type::is_separator (*s))
        ++s;
    }

    cs.append (s);
  }

  template <typename T>
  inline void
  append_options (cstrings& args, T& s, const variable& var, const char* e)
  {
    append_options (args, s[var], e);
  }

  template <typename T>
  inline void
  append_options (strings& args, T& s, const variable& var, const char* e)
  {
    append_options (args, s[var], e);
  }

  template <typename T>
  inline void
  append_options (sha256& csum, T& s, const variable& var)
  {
    append_options (csum, s[var]);
  }

  template <typename T>
  inline void
  append_options (cstrings& args, T& s, const char* var, const char* e)
  {
    append_options (args, s[var], e);
  }

  template <typename T>
  inline void
  append_options (strings& args, T& s, const char* var, const char* e)
  {
    append_options (args, s[var], e);
  }

  template <typename T>
  inline void
  append_options (sha256& csum, T& s, const char* var)
  {
    append_options (csum, s[var]);
  }

  inline void
  append_options (cstrings& args, const strings& sv, const char* e)
  {
    if (size_t n = sv.size ())
      append_options (args, sv, n, e);
  }

  inline void
  append_options (strings& args, const strings& sv, const char* e)
  {
    if (size_t n = sv.size ())
      append_options (args, sv, n, e);
  }

  inline void
  append_options (sha256& csum, const strings& sv)
  {
    if (size_t n = sv.size ())
      append_options (csum, sv, n);
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
  find_options (const initializer_list<const char*>& os,
                T& s,
                const variable& var,
                bool ic)
  {
    return find_options (os, s[var], ic);
  }

  template <typename T>
  inline bool
  find_options (const initializer_list<const char*>& os,
                T& s,
                const char* var,
                bool ic)
  {
    return find_options (os, s[var], ic);
  }

  template <typename T>
  inline const string*
  find_option_prefix (const char* p, T& s, const variable& var, bool ic)
  {
    return find_option_prefix (p, s[var], ic);
  }

  template <typename T>
  inline const string*
  find_option_prefix (const char* p, T& s, const char* var, bool ic)
  {
    return find_option_prefix (p, s[var], ic);
  }

  template <typename T>
  inline const string*
  find_option_prefixes (const initializer_list<const char*>& ps,
                        T& s,
                        const variable& var,
                        bool ic)
  {
    return find_option_prefixes (ps, s[var], ic);
  }

  template <typename T>
  inline const string*
  find_option_prefixes (const initializer_list<const char*>& ps,
                        T& s,
                        const char* var,
                        bool ic)
  {
    return find_option_prefixes (ps, s[var], ic);
  }

  inline size_t
  find_stem (const string& s, size_t s_p, size_t s_n,
             const char* stem, const char* seps)
  {
    auto sep = [seps] (char c) -> bool
    {
      return strchr (seps, c) != nullptr;
    };

    size_t m (strlen (stem));
    size_t p (s.find (stem, s_p, m));

    return (p != string::npos &&
            (      p == s_p || sep (s[p - 1])) && // Separated beginning.
            ((p + m) == s_n || sep (s[p + m])))   // Separated end.
      ? p
      : string::npos;
  }
}
