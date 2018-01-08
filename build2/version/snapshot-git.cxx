// file      : build2/version/snapshot-git.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbutl/sha1.mxx>

#include <build2/version/snapshot.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace version
  {
    snapshot
    extract_snapshot_git (const dir_path& src_root)
    {
      snapshot r;
      const char* d (src_root.string ().c_str ());

      // First check whether the working directory is clean. There doesn't
      // seem to be a way to do everything in a single invocation (the
      // porcelain v2 gives us the commit id but not timestamp).
      //

      // If git status --porcelain returns anything, then the working
      // directory is not clean.
      //
      {
        const char* args[] {"git", "-C", d, "status", "--porcelain", nullptr};

        if (!run<string> (3, args, [](string& s) {return move (s);}).empty ())
          return r;
      }

      // Now extract the commit id and date. One might think that would be
      // easy... Commit id is a SHA1 hash of the commit object. And commit
      // object looks like this:
      //
      // commit <len>\0
      // <data>
      //
      // Where <len> is the size of <data> and <data> is the output of:
      //
      // git cat-file commit ...
      //
      string data;

      const char* args[] {
        "git", "-C", d, "cat-file", "commit", "HEAD", nullptr};
      process pr (run_start (3     /* verbosity */,
                             args,
                             0     /* stdin */,
                             -1    /* stdout */));

      try
      {
        ifdstream is (move (pr.in_ofd), ifdstream::badbit);

        for (string l; !eof (getline (is, l)); )
        {
          data += l;
          data += '\n'; // We assume there is always a newline.

          if (r.sn == 0 && l.compare (0, 10, "committer ") == 0)
          try
          {
            // The line format is:
            //
            // committer <noise> <timestamp> <timezone>
            //
            // For example:
            //
            // committer John Doe <john@example.org> 1493117819 +0200
            //
            // The timestamp is in seconds since UNIX epoch. The timezone
            // appears to be always numeric (+0000 for UTC).
            //
            size_t p1 (l.rfind (' ')); // Can't be npos.
            string tz (l, p1 + 1);

            size_t p2 (l.rfind (' ', p1 - 1));
            if (p2 == string::npos)
              throw invalid_argument ("missing timestamp");

            string ts (l, p2 + 1, p1 - p2 - 1);
            r.sn = stoull (ts);

            if (tz.size () != 5)
              throw invalid_argument ("invalid timezone");

            unsigned long h (stoul (string (tz, 1, 2)));
            unsigned long m (stoul (string (tz, 3, 2)));
            unsigned long s (h * 3600 + m * 60);

            // The timezone indicates where the timestamp was generated so to
            // convert to UTC we need to invert the sign.
            //
            switch (tz[0])
            {
            case '+': r.sn -= s; break;
            case '-': r.sn += s; break;
            default: throw invalid_argument ("invalid timezone sign");
            }
          }
          catch (const invalid_argument& e)
          {
            fail << "unable to extract git commit date from '" << l << "': "
                 << e;
          }
        }

        is.close ();
      }
      catch (const io_error&)
      {
        // Presumably the child process failed. Let run_finish() deal with
        // that.
      }

      run_finish (args, pr);

      if (r.sn == 0)
        fail << "unable to extract git commit id/date for " << src_root;

      sha1 cs;
      cs.append ("commit " + to_string (data.size ())); // Includes '\0'.
      cs.append (data.c_str (), data.size ());

      r.id.assign (cs.string (), 16); // 16-characters abbreviated commit id.

      return r;
    }
  }
}
