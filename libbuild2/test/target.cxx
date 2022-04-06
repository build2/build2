// file      : libbuild2/test/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/target.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    static const char*
    testscript_target_extension (const target_key& tk, const scope*)
    {
      // If the name is special 'testscript', then there is no extension,
      // otherwise it is .testscript.
      //
      return *tk.name == "testscript" ? "" : "testscript";
    }

    static bool
    testscript_target_pattern (const target_type&,
                               const scope&,
                               string& v,
                               optional<string>& e,
                               const location& l,
                               bool r)
    {
      if (r)
      {
        assert (e);
        e = nullopt;
      }
      else
      {
        e = target::split_name (v, l);

        if (!e && v != "testscript")
        {
          e = "testscript";
          return true;
        }
      }

      return false;
    }

    const target_type testscript::static_type
    {
      "testscript",
      &file::static_type,
      &target_factory<testscript>,
      &testscript_target_extension,
      nullptr,  /* default_extension */
      &testscript_target_pattern,
      nullptr,
      &file_search,
      target_type::flag::none
    };
  }
}
