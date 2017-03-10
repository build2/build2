// file      : build2/test/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static pair<target*, optional<string>>
    testscript_factory (const target_type&,
                        dir_path d,
                        dir_path o,
                        string n,
                        optional<string> e)
    {
      if (!e)
        e = (n == "testscript" ? string () : "test");

      return make_pair (
        new testscript (move (d), move (o), move (n)), move (e));
    }

    static optional<string>
    testscript_target_extension (const target_key& tk, const scope&, bool)
    {
      // If the name is special 'testscript', then there is no extension,
      // otherwise it is .test.
      //
      return *tk.name == "testscript" ? string () : "test";
    }

    static bool
    testscript_target_pattern (const target_type&,
                               const scope&,
                               string& v,
                               bool r)
    {
      size_t p (path::traits::find_extension (v));

      if (r)
      {
        assert (p != string::npos);
        v.resize (p);
      }
      else if (p == string::npos && v != "testscript")
      {
        v += ".test";
        return true;
      }

      return false;
    }

    const target_type testscript::static_type
    {
      "test",
      &file::static_type,
      &testscript_factory,
      &testscript_target_extension,
      &testscript_target_pattern,
      nullptr,
      &search_file,
      false
    };
  }
}
