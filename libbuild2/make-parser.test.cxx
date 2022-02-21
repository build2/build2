// file      : libbuild2/make-parser.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/make-parser.hxx>
#include <libbuild2/diagnostics.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;

namespace build2
{
  int
  main (int, char* argv[])
  {
    // Fake build system driver, default verbosity.
    //
    init_diag (1);
    init (nullptr, argv[0], true);

    path_name in ("<stdin>");

    try
    {
      cin.exceptions (istream::badbit);

      using make_state = make_parser;
      using make_type = make_parser::type;

      make_parser make;

      location ll (in, 1);
      for (string l; !eof (getline (cin, l)); ++ll.line)
      {
        if (make.state == make_state::end)
        {
          cout << endl;
          make.state = make_state::begin;
        }

        // Skip leading blank lines to reduce output noise.
        //
        if (make.state == make_state::begin && l.empty ())
          continue;

        size_t pos (0);
        do
        {
          pair<make_type, path> r (make.next (l, pos, ll));

          cout << (r.first == make_type::target ? 'T' : 'P');

          if (!r.second.empty ())
            cout << ' ' << r.second;

          cout << endl;
        }
        while (pos != l.size ());
      }

      if (make.state != make_state::end && make.state != make_state::begin)
        fail (ll) << "incomplete make dependency declaration";
    }
    catch (const io_error& e)
    {
      cerr << "unable to read stdin: " << e << endl;
      return 1;
    }
    catch (const failed&)
    {
      return 1;
    }

    return 0;
  }
}

int
main (int argc, char* argv[])
{
  return build2::main (argc, argv);
}
