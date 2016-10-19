// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/runner>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      static ostream&
      operator<< (ostream& o, const test& t)
      {
        auto print_string = [&o] (const string& s)
        {
          // Quote if empty or contains spaces.
          //
          if (s.empty () || s.find (' ') != string::npos)
            o << '"' << s << '"';
          else
            o << s;
        };

        auto print_redirect = [&o, &print_string] (const redirect& r,
                                                   const char* prefix)
        {
          o << ' ' << prefix;

          size_t n (string::traits_type::length (prefix));
          assert (n > 0);

          switch (r.type)
          {
          case redirect_type::null: o << '!'; break;
          case redirect_type::here_string: print_string (r.value); break;
          case redirect_type::here_document:
            {
              o << prefix[n - 1]; // Add another '>' or '<'.
              print_string (r.here_end);
              break;
            }
          default: assert (false);
          }
        };

        auto print_heredoc = [&o] (const redirect& r)
        {
          // Here-document value always ends with a newline.
          //
          o << endl << r.value << r.here_end;
        };

        print_string (t.program.string ());

        for (const auto& a: t.arguments)
        {
          o << ' ';
          print_string (a);
        }

        if (t.in.type != redirect_type::none)
          print_redirect (t.in,  "<");

        if (t.out.type != redirect_type::none)
          print_redirect (t.out, ">");

        if (t.err.type != redirect_type::none)
          print_redirect (t.err, "2>");

        if (t.exit.comparison != exit_comparison::eq || t.exit.status != 0)
          o << (t.exit.comparison == exit_comparison::eq ? " == " : " != ")
            << (int)t.exit.status;

        if (t.in.type == redirect_type::here_document)
          print_heredoc (t.in);

        if (t.out.type == redirect_type::here_document)
          print_heredoc (t.out);

        if (t.err.type == redirect_type::here_document)
          print_heredoc (t.err);

        return o;
      }

      static void
      print_test (diag_record& r, const test& t)
      {
        // @@ No indentation performed for here-documents. If to fix then
        // probably need to do on diag_record level in a way similar to
        // butl::pager approach.
        //
        r << t;
      }

      static void
      print_test (const test& t)
      {
        diag_record r (text);
        print_test (r, t);
      }

      void concurrent_runner::
      run (const test& t)
      {
        // @@ TODO

        // @@ When running multiple threads will need to synchronize printing
        // the diagnostics so it don't overlap for concurrent tests.
        // Alternatively we can not bother with that and expect a user to
        // re-run test operation in the single-thread mode.
        //

        if (verb >= 3)
          print_test (t);
      }
    }
  }
}
