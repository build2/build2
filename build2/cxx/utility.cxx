// file      : build2/cxx/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/utility>

#include <build2/bin/target>

using namespace std;

namespace build2
{
  namespace cxx
  {
    // Return true if there is an option (stored in s).
    //
    bool
    translate_std (scope& rs, const string& cid, const value& val, string& s)
    {
      const string& v (cast<string> (val));

      if (cid == "msvc")
      {
        // C++ standard-wise, with VC++ you get what you get. The question is
        // whether we should verify that the requested standard is provided by
        // this VC++ version. And if so, from which version should we say VC++
        // supports 11, 14, and 17? We should probably be as loose as possible
        // here since the author will always be able to tighten (but not
        // loosen) this in the buildfile (i.e., detect unsupported versions).
        //
        // For now we are not going to bother doing this for C++03.
        //
        if (v != "98" && v != "03")
        {
          uint64_t cver (cast<uint64_t> (rs["cxx.version.major"]));

          // @@ Is mapping for 14 and 17 correct? Maybe Update 2 for 14?
          //
          if ((v == "11" && cver < 16) || // C++11 since VS2010/10.0.
              (v == "14" && cver < 19) || // C++14 since VS2015/14.0.
              (v == "17" && cver < 20))   // C++17 since VS20??/15.0.
          {
            fail << "C++" << v << " is not supported by "
                 << cast<string> (rs["cxx.signature"]) <<
              info << "required by " << rs.out_path ();
          }
        }

        return false;
      }
      else
      {
        // Translate 11 to 0x, 14 to 1y, and 17 to 1z for compatibility with
        // older versions of the compilers.
        //
        s = "-std=";

        if (v == "98")
          s += "c++98";
        else if (v == "03")
          s += "c++03";
        else if (v == "11")
          s += "c++0x";
        else if (v == "14")
          s += "c++1y";
        else if (v == "17")
          s += "c++1z";
        else
          s += v; // In case the user specifies something like 'gnu++17'.

        return true;
      }
    }

    void
    append_lib_options (cstrings& args, target& l, const char* var)
    {
      using namespace bin;

      for (target* t: l.prerequisite_targets)
      {
        if (t->is_a<lib> () || t->is_a<liba> () || t->is_a<libs> ())
          append_lib_options (args, *t, var);
      }

      append_options (args, l, var);
    }

    void
    hash_lib_options (sha256& csum, target& l, const char* var)
    {
      using namespace bin;

      for (target* t: l.prerequisite_targets)
      {
        if (t->is_a<lib> () || t->is_a<liba> () || t->is_a<libs> ())
          hash_lib_options (csum, *t, var);
      }

      hash_options (csum, l, var);
    }
  }
}
