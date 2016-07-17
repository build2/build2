// file      : build2/cxx/windows-rpath.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <errno.h> // E*

#include <set>
#include <fstream>

#include <build2/scope>
#include <build2/context>
#include <build2/variable>
#include <build2/filesystem>
#include <build2/diagnostics>

#include <build2/bin/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
  {
    // Provide limited emulation of the rpath functionality on Windows using a
    // side-by-side assembly. In a nutshell, the idea is to create an assembly
    // with links to all the prerequisite DLLs.
    //
    // Note that currently our assemblies contain all the DLLs that the
    // executable depends on, recursively. The alternative approach could be
    // to also create assemblies for DLLs. This appears to be possible (but we
    // will have to use the resource ID 2 for such a manifest). And it will
    // probably be necessary for DLLs that are loaded dynamically with
    // LoadLibrary(). The tricky part is how such nested assemblies will be
    // found. Since we are effectively (from the loader's point of view)
    // copying the DLLs, we will also have to copy their assemblies (because
    // the loader looks for them in the same directory as the DLL). It's not
    // clear how well such nested assemblies are supported (e.g., in Wine).
    //
    using namespace bin;

    // Return the greatest (newest) timestamp of all the DLLs that we will be
    // adding to the assembly or timestamp_nonexistent if there aren't any.
    //
    timestamp
    windows_rpath_timestamp (file& t)
    {
      timestamp r (timestamp_nonexistent);

      for (target* pt: t.prerequisite_targets)
      {
        if (libs* ls = pt->is_a<libs> ())
        {
          // This can be an installed library in which case we will have just
          // the import stub but may also have just the DLL. For now we don't
          // bother with installed libraries.
          //
          if (ls->member == nullptr)
            continue;

          file& dll (static_cast<file&> (*ls->member));

          // What if the DLL is in the same directory as the executable, will
          // it still be found even if there is an assembly? On the other
          // hand, handling it as any other won't hurt us much.
          //
          timestamp t;

          if ((t = dll.mtime ()) > r)
            r = t;

          if ((t = windows_rpath_timestamp (*ls)) > r)
            r = t;
        }
      }

      return r;
    }

    // Like *_timestamp() but actually collect the DLLs.
    //
    static void
    rpath_dlls (set<file*>& s, file& t)
    {
      for (target* pt: t.prerequisite_targets)
      {
        if (libs* ls = pt->is_a<libs> ())
        {
          if (ls->member == nullptr)
            continue;

          file& dll (static_cast<file&> (*ls->member));

          s.insert (&dll);
          rpath_dlls (s, *ls);
        }
      }
    }

    const char*
    windows_manifest_arch (const string& tcpu); // windows-manifest.cxx

    // The ts argument should be the the DLLs timestamp returned by
    // *_timestamp().
    //
    // The scratch argument should be true if the DLL set has changed and we
    // need to regenerate everything from scratch. Otherwise, we try to avoid
    // unnecessary work by comparing the DLLs timestamp against the assembly
    // manifest file.
    //
    void
    windows_rpath_assembly (file& t, timestamp ts, bool scratch)
    {
      // Assembly paths and name.
      //
      dir_path ad (path_cast<dir_path> (t.path () + ".dlls"));
      string an (ad.leaf ().string ());
      path am (ad / path (an + ".manifest"));

      // First check if we actually need to do anything. Since most of the
      // time we won't, we don't want to combine it with the *_dlls() call
      // below which allocates memory, etc.
      //
      if (!scratch)
      {
        // The corner case here is when _timestamp() returns nonexistent
        // signalling that there aren't any DLLs but the assembly manifest
        // file exists. This, however, can only happen if we somehow managed
        // to transition from the "have DLLs" state to "no DLLs" without going
        // through the "from scratch" update. And this shouldn't happen
        // (famous last words before a core dump).
        //
        if (ts <= file_mtime (am))
          return;
      }

      scope& rs (t.root_scope ());

      // Next collect the set of DLLs that will be in our assembly. We need to
      // do this recursively which means we may end up with duplicates. Also,
      // it is possible that there aren't/no longer are any DLLs which means
      // we just need to clean things up.
      //
      bool empty (ts == timestamp_nonexistent);

      set<file*> dlls;
      if (!empty)
        rpath_dlls (dlls, t);

      // Clean the assembly directory and make sure it exists. Maybe it would
      // have been faster to overwrite the existing manifest rather than
      // removing the old one and creating a new one. But this is definitely
      // simpler.
      //
      {
        rmdir_status s (build2::rmdir_r (ad, empty, 3));

        if (empty)
          return;

        if (s == rmdir_status::not_exist)
          mkdir (ad, 3);
      }

      const char* pa (
        windows_manifest_arch (
          cast<string> (rs["cxx.target.cpu"])));

      if (verb >= 3)
        text << "cat >" << am;

      try
      {
        ofstream ofs;
        ofs.exceptions (ofstream::failbit | ofstream::badbit);
        ofs.open (am.string ());

        ofs << "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
            << "<assembly xmlns='urn:schemas-microsoft-com:asm.v1'\n"
            << "          manifestVersion='1.0'>\n"
            << "  <assemblyIdentity name='" << an << "'\n"
            << "                    type='win32'\n"
            << "                    processorArchitecture='" << pa << "'\n"
            << "                    version='0.0.0.0'/>\n";

        scope& as (*rs.weak_scope ()); // Amalgamation scope.

        for (file* dt: dlls)
        {
          const path& dp (dt->path ()); // DLL path.
          const path dn (dp.leaf ());   // DLL name.
          const path lp (ad / dn);      // Link path.

          auto print = [&dp, &lp] (const char* cmd)
          {
            if (verb >= 3)
              text << cmd << ' ' << dp << ' ' << lp;
          };

          // First we try to create a symlink. If that fails (e.g., "Windows
          // happens"), then we resort to hard links. If that doesn't work
          // out either (e.g., not on the same filesystem), then we fall back
          // to copies. So things are going to get a bit nested.
          //
          try
          {
            // For the symlink use a relative target path if both paths are
            // part of the same amalgamation. This way if the amalgamation is
            // moved as a whole, the links will remain valid.
            //
            if (dp.sub (as.out_path ()))
              mksymlink (dp.relative (ad), lp);
            else
              mksymlink (dp, lp);

            print ("ln -s");
          }
          catch (const system_error& e)
          {
            int c (e.code ().value ());

            if (c != EPERM && c != ENOSYS)
            {
              print ("ln -s");
              fail << "unable to create symlink " << lp << ": " << e.what ();
            }

            try
            {
              mkhardlink (dp, lp);
              print ("ln");
            }
            catch (const system_error& e)
            {
              int c (e.code ().value ());

              if (c != EPERM && c != ENOSYS)
              {
                print ("ln");
                fail << "unable to create hard link " << lp << ": "
                     << e.what ();
              }

              try
              {
                cpfile (dp, lp);
                print ("cp");
              }
              catch (const system_error& e)
              {
                print ("cp");
                fail << "unable to create copy " << lp << ": " << e.what ();
              }
            }
          }

          ofs << "  <file name='" << dn.string () << "'/>\n";
        }

        ofs << "</assembly>\n";
      }
      catch (const ofstream::failure&)
      {
        fail << "unable to write to " << am;
      }
    }
  }
}
