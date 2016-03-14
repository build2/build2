// file      : build2/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  template <typename T>
  T
  run (const char* const* args,
       T (*f) (string&),
       bool err,
       bool ignore_exit,
       sha256* checksum)
  {
    process pr (start_run (args, err));
    ifdstream is (pr.in_ofd);

    T r;

    string l; // Last line of output.
    while (is.peek () != ifdstream::traits_type::eof () && // Keep last line.
           getline (is, l))
    {
      trim (l);

      if (checksum != nullptr)
        checksum->append (l);

      if (r.empty ())
        r = f (l);
    }

    is.close (); // Don't block.

    if (!(finish_run (args, err, pr, l) || ignore_exit))
      r = T ();

    return r;
  }
}
