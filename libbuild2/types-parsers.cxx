// file      : libbuild2/types-parsers.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/types-parsers.hxx>

#include <sstream>

#include <libbuild2/lexer.hxx>
#include <libbuild2/parser.hxx>

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

      void parser<name>::
      parse (name& x, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (!s.more ())
          throw missing_value (o);

        const char* v (s.next ());

        try
        {
          using build2::parser;
          using std::istringstream;

          istringstream is (v);
          is.exceptions (istringstream::failbit | istringstream::badbit);

          // @@ TODO: currently this issues diagnostics to diag_stream.
          //          Perhaps we should redirect it?
          //
          path_name in (o);
          lexer l (is, in, 1 /* line */, "\'\"\\$("); // Effective.
          parser p (nullptr);
          names r (p.parse_names (l, nullptr, parser::pattern_mode::preserve));

          if (r.size () != 1)
            throw invalid_value (o, v);

          x = move (r.front ());
          xs = true;
        }
        catch (const failed&)
        {
          throw invalid_value (o, v);
        }
      }

      void parser<structured_result_format>::
      parse (structured_result_format& x, bool& xs, scanner& s)
      {
        xs = true;
        const char* o (s.next ());

        if (!s.more ())
          throw missing_value (o);

        const string v (s.next ());
        if (v == "lines")
          x = structured_result_format::lines;
        else if (v == "json")
          x = structured_result_format::json;
        else
          throw invalid_value (o, v);
      }
    }
  }
}
