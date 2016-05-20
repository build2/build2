// file      : build2/cxx/utility.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace cxx
  {
    bool
    translate_std (scope&, const string&, const value&, string&);

    template <typename T>
    inline void
    append_std (cstrings& args, scope& rs, const string& cid, T& t, string& s)
    {
      if (auto l = t["cxx.std"])
        if (translate_std (rs, cid, *l, s))
          args.push_back (s.c_str ());
    }

    template <typename T>
    inline void
    hash_std (sha256& csum, scope& rs, const string& cid, T& t)
    {
      if (auto l = t["cxx.std"])
      {
        string s;
        if (translate_std (rs, cid, *l, s))
          csum.append (s);
      }
    }
  }
}
