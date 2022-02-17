// file      : libbuild2/types-parsers.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/types-parsers.hxx>

#include <libbuild2/b-options.hxx> // build2::build::cli namespace

namespace build2
{
  namespace build
  {
    namespace cli
    {
      template <typename T>
      static void
      parse_path (T& x, scanner& s)
      {
        const char* o (s.next ());

        if (!s.more ())
          throw missing_value (o);

        const char* v (s.next ());

        try
        {
          x = T (v);

          if (x.empty ())
            throw invalid_value (o, v);
        }
        catch (const invalid_path&)
        {
          throw invalid_value (o, v);
        }
      }

      void parser<path>::
      parse (path& x, bool& xs, scanner& s)
      {
        xs = true;
        parse_path (x, s);
      }

      void parser<dir_path>::
      parse (dir_path& x, bool& xs, scanner& s)
      {
        xs = true;
        parse_path (x, s);
      }
    }
  }
}
