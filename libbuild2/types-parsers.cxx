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

      static names
      parse_names (const char* o, const char* v)
      {
        using build2::parser;
        using std::istringstream;

        istringstream is (v);
        is.exceptions (istringstream::failbit | istringstream::badbit);

        // @@ TODO: currently this issues diagnostics to diag_stream.
        //          Perhaps we should redirect it? Also below.
        //
        path_name in (o);
        lexer l (is, in, 1 /* line */, "\'\"\\$("); // Effective.
        parser p (nullptr);
        return p.parse_names (l, nullptr, parser::pattern_mode::preserve);
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
          names r (parse_names (o, v));

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

      void parser<pair<name, optional<name>>>::
      parse (pair<name, optional<name>>& x, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (!s.more ())
          throw missing_value (o);

        const char* v (s.next ());

        try
        {
          names r (parse_names (o, v));

          if (r.size () == 1)
          {
            x.first = move (r.front ());
            x.second = nullopt;
          }
          else if (r.size () == 2 && r.front ().pair == '@')
          {
            x.first = move (r.front ());
            x.second = move (r.back ());
          }
          else
            throw invalid_value (o, v);

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
