// file      : libbuild2/bin/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_UTILITY_HXX
#define LIBBUILD2_BIN_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>

namespace build2
{
  namespace bin
  {
    // Lookup the bin.pattern value and split it into the pattern and the
    // search paths.
    //
    struct pattern_paths
    {
      const char* pattern = nullptr;
      const char* paths   = nullptr;
    };

    inline pattern_paths
    lookup_pattern (const scope& rs)
    {
      pattern_paths r;

      // Theoretically, we could have both the pattern and the search paths,
      // for example, the pattern can come first followed by the paths.
      //
      if (const string* v = cast_null<string> (rs["bin.pattern"]))
      {
        (path::traits_type::is_separator (v->back ())
         ? r.paths
         : r.pattern) = v->c_str ();
      }

      return r;
    }
  }
}

#endif // LIBBUILD2_BIN_UTILITY_HXX
