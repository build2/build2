// file      : build2/version/snapshot-git.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/snapshot.hxx>

using namespace std;

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

        if (!run<string> (args, [] (string& s) {return move (s);}).empty ())
          return r;
      }

      // Now extract the commit id and date.
      //
      auto extract = [&r] (string& s) -> snapshot
      {
        if (s.compare (0, 5, "tree ") == 0)
        {
          // The 16-characters abbreviated commit id.
          //
          r.id.assign (s, 5, 16);

          if (r.id.size () != 16)
            fail << "unable to extract git commit id from '" << s << "'";
        }
        else if (s.compare (0, 10, "committer ") == 0)
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
          size_t p1 (s.rfind (' ')); // Can't be npos.
          string tz (s, p1 + 1);

          size_t p2 (s.rfind (' ', p1 - 1));
          if (p2 == string::npos)
            throw invalid_argument ("missing timestamp");

          string ts (s, p2 + 1, p1 - p2 - 1);
          r.sn = stoull (ts);

          if (tz.size () != 5)
            throw invalid_argument ("invalid timezone");

          unsigned long h (stoul (string (tz, 1, 2)));
          unsigned long m (stoul (string (tz, 3, 2)));
          unsigned long s (h * 3600 + m * 60);

          // The timezone indicates where the timestamp was generated so
          // to convert to UTC we need to invert the sign.
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
          fail << "unable to extract git commit date from '" << s << "': " << e;
        }

        return (r.id.empty () || r.sn == 0) ? snapshot () : move (r);
      };

      const char* args[] {
        "git", "-C", d, "cat-file", "commit", "HEAD", nullptr};
      r = run<snapshot> (args, extract);

      if (r.empty ())
        fail << "unable to extract git commit id/date for " << src_root;

      return r;
    }
  }
}
