// file      : build2/test/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static target*
    testscript_factory (const target_type&,
                        dir_path d,
                        dir_path o,
                        string n,
                        const string* e)
    {
      if (e == nullptr)
        e = &extension_pool.find (n == "testscript" ? "" : "test");

      return new testscript (move (d), move (o), move (n), e);
    }

    static const string*
    testscript_target_extension (const target_key& tk, scope&)
    {
      // If the name is special 'testscript', then there is no extension,
      // otherwise it is .test.
      //
      return &extension_pool.find (*tk.name == "testscript" ? "" : "test");
    }

    const target_type testscript::static_type
    {
      "test",
      &file::static_type,
      &testscript_factory,
      &testscript_target_extension,
      nullptr,
      &search_file,
      false
    };
  }
}
