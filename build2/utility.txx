// file      : build2/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  template <typename K>
  basic_path<char, K>
  relative (const basic_path<char, K>& p)
  {
    typedef basic_path<char, K> path;

    const dir_path& b (*relative_base);

    if (p.simple () || b.empty ())
      return p;

    if (p.sub (b))
      return p.leaf (b);

    if (p.root_directory () == b.root_directory ())
    {
      path r (p.relative (b));

      if (r.string ().size () < p.string ().size ())
        return r;
    }

    return p;
  }

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
      ifdstream is (move (pr.in_ofd));

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
