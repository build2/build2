// file      : build2/test/script/builtin.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/builtin>

#include <butl/fdstream>

using namespace std;
using namespace butl;

namespace build2
{
  //@@ auto_fd handover
  //
  // 1. We need auto_fd in libbutl
  // 2. Overload fdstream ctors for auto_fd&& (or replace? also process data
  //    members?)
  // 3. The builtin signature then will become:
  //
  // static int
  // echo (const strings& args, auto_fd in, auto_fd out, auto_fd err)

  static int
  echo (const strings& args, int in_fd, int out_fd, int err_fd)
  try
  {
    int r (0);
    ofdstream cerr (err_fd);

    try
    {
      fdclose (in_fd); //@@ TMP
      ofdstream cout (out_fd);

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
