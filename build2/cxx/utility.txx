// file      : build2/cxx/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

using namespace std;

namespace build2
{
  namespace cxx
  {
    template <typename T>
    void
    append_std (cstrings& args, T& t, std::string& s)
    {
      if (auto l = t["cxx.std"])
      {
        const std::string& v (as<string> (*l));

        // Translate 11 to 0x and 14 to 1y for compatibility with
        // older versions of the compiler.
        //
        s = "-std=c++";

        if (v == "11")
          s += "0x";
        else if (v == "14")
          s += "1y";
        else
          s += v;

        args.push_back (s.c_str ());
      }
    }
  }
}
