// file      : libbuild2/version/snapshot-git.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <ctime> // time_t

#include <libbutl/sha1.hxx>

#include <libbuild2/version/snapshot.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace version
  {
    // We have to run git twice to extract the information we need and doing
    // it repetitively is quite expensive, especially for larger repositories.
    // So we cache it, which helps multi-package repositories.
    //
    static global_cache<snapshot, dir_path> cache;

    snapshot
    extract_snapshot_git (context& ctx, dir_path rep_root)
    {
      if (const snapshot* r = cache.find (rep_root))
        return *r;

      snapshot r;
      const char* d (rep_root.string ().c_str ());

      // On startup git prepends the PATH environment variable value with the
      // computed directory path where its sub-programs are supposedly located
      // (--exec-path option, GIT_EXEC_PATH environment variable, etc; see
      // cmd_main() in git's git.c for details).
      //
      // Then, when git needs to run itself or one of its components as a
      // child process, it resolves the full executable path searching in
      // directories listed in PATH (see locate_in_PATH() in git's
      // run-command.c for details).
      //
      // On Windows we install git and its components into a place where it is
      // not expected to be, which results in the wrong path in PATH as set by
      // git (for example, c:/build2/libexec/git-core) which in turn may lead
      // to running some other git that appear in the PATH variable. To
      // prevent this we pass the git's exec directory via the --exec-path
      // option explicitly.
      //
      // Note also that git has quite a few GIT_* environment variables and
      // stray values for some of them could break our commands. So it may
      // seem like a good idea to unset them. But on the other hand, they may
      // be there for a reason: after all, we are operating on user's projects
      // and user's environment may be setup to handle them.
      //
      path p ("git");
      process_path pp (run_search (p, true /* init */));

#ifdef _WIN32
      string ep ("--exec-path=" + pp.effect.directory ().string ());
#endif

      size_t args_i (3); // First reserved.
      const char* args[] {
        pp.recall_string (),
#ifdef _WIN32
        (++args_i, ep.c_str ()),
#endif
        "-C",
        d,
        nullptr, nullptr, nullptr, // Reserve.
        nullptr};

      // First check whether the working directory is clean. There doesn't
      // seem to be a way to do everything in a single invocation (the
      // porcelain v2 gives us the commit id but not timestamp).
      //

      // If git status --porcelain returns anything, then the working
      // directory is not clean.
      //
      args[args_i    ] = "status";
      args[args_i + 1] = "--porcelain";
      args[args_i + 2] = nullptr;

      // @@ PERF: redo with custom stream reading code (then could also
      //    get rid of context).
      //
      r.committed = run<string> (
        ctx,
        3 /* verbosity */,
        pp,
        args,
        [](string& s, bool) {return move (s);}).empty ();

      // Now extract the commit id and date. One might think that would be
      // easy... Commit id is a SHA1 hash of the commit object. And commit
      // object looks like this:
      //
      // commit <len>\0
      // <data>
      //
      // Where <len> is the size of <data> and <data> is the output of:
      //
      // git cat-file commit HEAD
      //
      // There is also one annoying special case: new repository without any
      // commits. In this case the above command will fail (with diagnostics
      // and non-zero exit code) because there is no HEAD. Of course, it can
      // also fail for other reason (like broken repository) which would be
      // hard to distinguish. Note, however, that we just ran git status and
      // it would have most likely failed if this were the case. So here we
      // (reluctantly) assume that the only reason git cat-file fails is if
      // there is no HEAD (that we equal with the "new repository" condition
      // which is, strictly speaking, might not be the case either). So we
      // suppress any diagnostics, and handle non-zero exit code (and so no
      // diagnostics buffering is needed, plus we are in the load phase).
      //
      string data;

      args[args_i    ] = "cat-file";
      args[args_i + 1] = "commit";
      args[args_i + 2] = "HEAD";
      args[args_i + 3] = nullptr;

      process pr (run_start (3       /* verbosity */,
                             pp,
                             args,
                             0       /* stdin  */,
                             -1      /* stdout */,
                             1       /* stderr (to stdout) */));

      string l;
      try
      {
        ifdstream is (move (pr.in_ofd), ifdstream::badbit);

        while (!eof (getline (is, l)))
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
            // appears to be always numeric (+0000 for UTC). Note that
            // timestamp appears to be already in UTC with timezone being just
            // for information it seems.
            //
            size_t p1 (l.rfind (' ')); // Can't be npos.

            size_t p2 (l.rfind (' ', p1 - 1));
            if (p2 == string::npos)
              throw invalid_argument ("missing timestamp");

            string ts (l, p2 + 1, p1 - p2 - 1);
            time_t t (static_cast<time_t> (stoull (ts)));

#if 0
            string tz (l, p1 + 1);

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
            case '+': t -= s; break;
            case '-': t += s; break;
            default: throw invalid_argument ("invalid timezone sign");
            }
#endif
            // Represent as YYYYMMDDhhmmss.
            //
            r.sn = stoull (to_string (system_clock::from_time_t (t),
                                      "%Y%m%d%H%M%S",
                                      false /* special */,
                                      false /* local (already in UTC) */));
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

      if (run_finish_code (args, pr, l, 2 /* verbosity */))
      {
        if (r.sn == 0)
          fail << "unable to extract git commit id/date for " << rep_root;

        if (r.committed)
        {
          sha1 cs;
          cs.append ("commit " + to_string (data.size ())); // Includes '\0'.
          cs.append (data.c_str (), data.size ());
          r.id.assign (cs.string (), 12); // 12-char abbreviated commit id.
        }
        else
          r.sn++; // Add a second.
      }
      else
      {
        // Presumably new repository without HEAD. Return uncommitted snapshot
        // with UNIX epoch as timestamp.
        //
        r.sn = 19700101000000ULL;
        r.committed = false;
      }

      return cache.insert (move (rep_root), move (r));
    }
  }
}
