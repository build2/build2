// file      : build2/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  template <typename T>
  T
  run (const process_path& pp,
       const char* args[],
       T (*f) (string&),
       bool err,
       bool ignore_exit,
       sha256* checksum)
  {
    process pr (run_start (pp, args, err));

    T r;
    string l; // Last line of output.

    try
    {
      ifdstream is (pr.in_ofd);

      while (is.peek () != ifdstream::traits_type::eof () && // Keep last line.
             getline (is, l))
      {
        trim (l);

        if (checksum != nullptr)
          checksum->append (l);

        if (r.empty ())
          r = f (l);
      }
    }
    catch (const io_error&)
    {
      // Presumably the child process failed. Let run_finish() deal with that.
    }

    if (!(run_finish (args, err, pr, l) || ignore_exit))
      r = T ();

    return r;
  }
}
