// file      : build2/cxx/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

using namespace std;

namespace build2
{
  namespace cxx
  {
    template <typename T>
    bool
    translate_std (T& t, string& s)
    {
      if (auto l = t["cxx.std"])
      {
        const string& v (cast<string> (l));

        // Translate 11 to 0x and 14 to 1y for compatibility with older
        // versions of the compiler.
        //
        s = "-std=c++";

        if (v == "11")
          s += "0x";
        else if (v == "14")
          s += "1y";
        else
          s += v;

        return true;
      }

      return false;
    }

    template <typename T>
    inline void
    append_std (cstrings& args, T& t, string& s)
    {
      if (translate_std (t, s))
        args.push_back (s.c_str ());
    }

    template <typename T>
    inline void
    hash_std (sha256& csum, T& t)
    {
      string s;
      if (translate_std (t, s))
        csum.append (s);
    }
  }
}
