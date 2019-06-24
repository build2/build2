// file      : build2/types-parsers.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef BUILD2_TYPES_PARSERS_HXX
#define BUILD2_TYPES_PARSERS_HXX

#include <libbuild2/types.hxx>

namespace build2
{
  namespace cl
  {
    class scanner;

    template <typename T>
    struct parser;

    template <>
    struct parser<path>
    {
      static void
      parse (path&, bool&, scanner&);
    };

    template <>
    struct parser<dir_path>
    {
      static void
      parse (dir_path&, bool&, scanner&);
    };
  }
}

#endif // BUILD2_TYPES_PARSERS_HXX
