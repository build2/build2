// file      : build2/test/script/builtin.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/builtin>

#include <butl/fdstream>

using namespace std;
using namespace butl;

namespace build2
{
  static int
  echo (const strings& args, auto_fd, auto_fd out, auto_fd err)
  try
  {
    int r (0);
    ofdstream cerr (move (err));

    try
    {
      ofdstream cout (move (out));

      for (auto b (args.begin ()), i (b), e (args.end ()); i != e; ++i)
        cout << (i != b ? " " : "") << *i;

      cout << endl;

      cout.close ();
    }
    catch (const std::exception& e)
    {
      cerr << "echo: " << e.what ();
      r = 1;
    }

    cerr.close ();
    return r;
  }
  catch (const std::exception&)
  {
    return 1;
  }

  namespace test
  {
    namespace script
    {
      const builtin_map builtins
      {
        {"echo", &echo}
      };
    }
  }
}
