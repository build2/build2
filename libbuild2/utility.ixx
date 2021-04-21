// file      : libbuild2/utility.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cstring> // strlen() strchr()

namespace build2
{
  inline bool
  run_wait (cstrings& args, process& pr, const location& loc)
  {
    return run_wait (args.data (), pr, loc);
  }

  // Note: currently this function is also used in a run() implementations.
  //
  LIBBUILD2_SYMEXPORT bool
  run_finish_impl (const char*[],
                   process&,
                   bool error,
                   const string&,
                   const location& = location ());

  inline void
  run_finish (const char* args[],
              process& pr,
              const string& l,
              const location& loc)
  {
    run_finish_impl (args, pr, true /* error */, l, loc);
  }

  inline void
  run_finish (cstrings& args, process& pr, const location& loc)
  {
    run_finish (args.data (), pr, string (), loc);
  }

  inline bool
  run_finish_code (const char* args[],
                   process& pr,
                   const string& l,
                   const location& loc)
  {
    return run_finish_impl (args, pr, false /* error */, l, loc);
  }

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

  inline bool
  compare_option (const char* o, const char* s, bool ic)
  {
    return s != nullptr && (ic ? icasecmp (s, o) : strcmp (s, o)) == 0;
  }

  inline bool
  compare_option (const char* o, const string& s, bool ic)
  {
    return ic ? icasecmp (s, o) == 0 : s == o;
  }

  template <typename I>
  inline I
  find_option (const char* o, I b, I e, bool ic)
  {
    for (; b != e; ++b)
      if (compare_option (o, *b, ic))
        return b;

    return e;
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

  inline bool
  compare_option_prefix (const char* p, size_t n, const char* s, bool ic)
  {
    return s != nullptr && (ic ? icasecmp (s, p, n) : strncmp (s, p, n)) == 0;
  }

  inline bool
  compare_option_prefix (const char* p, size_t n, const string& s, bool ic)
  {
    return (ic ? icasecmp (s, p, n) : s.compare (0, n, p)) == 0;
  }

  template <typename I>
  inline I
  find_option_prefix (const char* p, I b, I e, bool ic)
  {
    size_t n (strlen (p));

    for (; b != e; ++b)
      if (compare_option_prefix (p, n, *b, ic))
        return b;

    return e;
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

  // hash_environment()
  //
  inline void
  hash_environment (sha256& cs, const char* n)
  {
    cs.append (n);

    if (optional<string> v = getenv (n))
      cs.append (*v);
  }

  inline void
  hash_environment (sha256& cs, const string& n)
  {
    hash_environment (cs, n.c_str ());
  }

  inline void
  hash_environment (sha256& cs, initializer_list<const char*> ns)
  {
    for (const char* n: ns)
      hash_environment (cs, n);
  }

  inline string
  hash_environment (initializer_list<const char*> ns)
  {
    sha256 cs;
    hash_environment (cs, ns);
    return cs.string ();
  }

  inline void
  hash_environment (sha256& cs, const cstrings& ns)
  {
    for (const char* n: ns)
      hash_environment (cs, n);
  }

  inline string
  hash_environment (const cstrings& ns)
  {
    sha256 cs;
    hash_environment (cs, ns);
    return cs.string ();
  }

  inline void
  hash_environment (sha256& cs, const strings& ns)
  {
    for (const string& n: ns)
      hash_environment (cs, n);
  }

  inline string
  hash_environment (const strings& ns)
  {
    sha256 cs;
    hash_environment (cs, ns);
    return cs.string ();
  }

  inline void
  hash_environment (sha256& cs, const char* const* ns)
  {
    if (ns != nullptr)
    {
      for (; *ns != nullptr; ++ns)
        hash_environment (cs, *ns);
    }
  }

  inline string
  hash_environment (const char* const* ns)
  {
    sha256 cs;
    hash_environment (cs, ns);
    return cs.string ();
  }

  // find_stem()
  //
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
