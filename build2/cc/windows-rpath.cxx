// file      : build2/cc/windows-rpath.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <errno.h> // E*

#include <build2/scope.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/link-rule.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
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
    // What if the DLL is in the same directory as the executable, will it
    // still be found even if there is an assembly? On the other hand,
    // handling it as any other won't hurt us much.
    //
    using namespace bin;

    // Return the greatest (newest) timestamp of all the DLLs that we will be
    // adding to the assembly or timestamp_nonexistent if there aren't any.
    //
    timestamp link_rule::
    windows_rpath_timestamp (const file& t,
                             const scope& bs,
                             action a,
                             linfo li) const
    {
      timestamp r (timestamp_nonexistent);

      // We need to collect all the DLLs, so go into implementation of both
      // shared and static (in case they depend on shared).
      //
      auto imp = [] (const file&, bool) {return true;};

      auto lib = [&r] (const file* const* lc,
                       const string& f,
                       lflags,
                       bool sys)
      {
        const file* l (lc != nullptr ? *lc : nullptr);

        // We don't rpath system libraries.
        //
        if (sys)
          return;

        // Skip static libraries.
        //
        if (l != nullptr)
        {
          // This can be an "undiscovered" DLL (see search_library()).
          //
          if (!l->is_a<libs> () || l->path ().empty ()) // Also covers binless.
            return;
        }
        else
        {
          // This is an absolute path and we need to decide whether it is
          // a shared or static library.
          //
          // @@ This is so broken: we don't link to DLLs, we link to .lib or
          //    .dll.a! Should we even bother? Maybe only for "our" DLLs
          //    (.dll.lib/.dll.a)? But the DLL can also be in a different
          //    directory (lib/../bin).
          //
          //    Though this can happen on MinGW with direct DLL link...
          //
          size_t p (path::traits::find_extension (f));

          if (p == string::npos || casecmp (f.c_str () + p + 1, "dll") != 0)
            return;
        }

        // Ok, this is a DLL.
        //
        timestamp t (l != nullptr
                     ? l->load_mtime ()
                     : file_mtime (f.c_str ()));

        if (t > r)
          r = t;
      };

      for (const prerequisite_target& pt: t.prerequisite_targets[a])
      {
        if (pt == nullptr || pt.adhoc)
          continue;

        bool la;
        const file* f;

        if ((la = (f = pt->is_a<liba>  ())) ||
            (la = (f = pt->is_a<libux> ())) || // See through.
            (     f = pt->is_a<libs>  ()))
          process_libraries (a, bs, li, sys_lib_dirs,
                             *f, la, pt.data,
                             imp, lib, nullptr, true);
      }

      return r;
    }

    // Like *_timestamp() but actually collect the DLLs (and weed out the
    // duplicates).
    //
    auto link_rule::
    windows_rpath_dlls (const file& t,
                        const scope& bs,
                        action a,
                        linfo li) const -> windows_dlls
    {
      windows_dlls r;

      auto imp = [] (const file&, bool) {return true;};

      auto lib = [&r, &bs] (const file* const* lc,
                            const string& f,
                            lflags,
                            bool sys)
      {
        const file* l (lc != nullptr ? *lc : nullptr);

        if (sys)
          return;

        if (l != nullptr)
        {
          if (l->is_a<libs> () && !l->path ().empty ()) // Also covers binless.
          {
            // Get .pdb if there is one.
            //
            const target_type* tt (bs.find_target_type ("pdb"));
            const target* pdb (tt != nullptr
                               ? find_adhoc_member (*l, *tt)
                               : nullptr);
            r.insert (
              windows_dll {
                f,
                pdb != nullptr ? &pdb->as<file> ().path ().string () : nullptr,
                string ()
              });
          }
        }
        else
        {
          size_t p (path::traits::find_extension (f));

          if (p != string::npos && casecmp (f.c_str () + p + 1, "dll") == 0)
          {
            // See if we can find a corresponding .pdb.
            //
            windows_dll wd {f, nullptr, string ()};
            string& pdb (wd.pdb_storage);

            // First try "our" naming: foo.dll.pdb.
            //
            pdb = f;
            pdb += ".pdb";

            if (!exists (path (pdb)))
            {
              // Then try the usual naming: foo.pdb.
              //
              pdb.assign (f, 0, p);
              pdb += ".pdb";

              if (!exists (path (pdb)))
                pdb.clear ();
            }

            if (!pdb.empty ())
              wd.pdb = &pdb;

            r.insert (move (wd));
          }
        }
      };

      for (const prerequisite_target& pt: t.prerequisite_targets[a])
      {
        if (pt == nullptr || pt.adhoc)
          continue;

        bool la;
        const file* f;

        if ((la = (f = pt->is_a<liba>  ())) ||
            (la = (f = pt->is_a<libux> ())) || // See through.
            (      f = pt->is_a<libs>  ()))
          process_libraries (a, bs, li, sys_lib_dirs,
                             *f, la, pt.data,
                             imp, lib, nullptr, true);
      }

      return r;
    }

    const char*
    windows_manifest_arch (const string& tcpu); // windows-manifest.cxx

    // The ts argument should be the DLLs timestamp returned by *_timestamp().
    //
    // The scratch argument should be true if the DLL set has changed and we
    // need to regenerate everything from scratch. Otherwise, we try to avoid
    // unnecessary work by comparing the DLLs timestamp against the assembly
    // manifest file.
    //
    void link_rule::
    windows_rpath_assembly (const file& t,
                            const scope& bs,
                            action a,
                            linfo li,
                            const string& tcpu,
                            timestamp ts,
                            bool scratch) const
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
        // through the "from scratch" update. Actually this can happen when
        // switching to update-for-install.
        //
        if (ts != timestamp_nonexistent && ts <= file_mtime (am))
          return;
      }

      // Next collect the set of DLLs that will be in our assembly. We need to
      // do this recursively which means we may end up with duplicates. Also,
      // it is possible that there aren't/no longer are any DLLs which means
      // we just need to clean things up.
      //
      bool empty (ts == timestamp_nonexistent);

      windows_dlls dlls;
      if (!empty)
        dlls = windows_rpath_dlls (t, bs, a, li);

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

      const char* pa (windows_manifest_arch (tcpu));

      if (verb >= 3)
        text << "cat >" << am;

      try
      {
        ofdstream ofs (am);

        ofs << "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
            << "<assembly xmlns='urn:schemas-microsoft-com:asm.v1'\n"
            << "          manifestVersion='1.0'>\n"
            << "  <assemblyIdentity name='" << an << "'\n"
            << "                    type='win32'\n"
            << "                    processorArchitecture='" << pa << "'\n"
            << "                    version='0.0.0.0'/>\n";

        const scope& as (*t.root_scope ().weak_scope ()); // Amalgamation.

        auto link = [&as, &ad] (const path& f, const path& l)
        {
          auto print = [&f, &l] (const char* cmd)
          {
            if (verb >= 3)
              text << cmd << ' ' << f << ' ' << l;
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
            if (f.sub (as.out_path ()))
              mksymlink (f.relative (ad), l);
            else
              mksymlink (f, l);

            print ("ln -s");
          }
          catch (const system_error& e)
          {
            // Note that we are not guaranteed (here and below) that the
            // system_error exception is of the generic category.
            //
            int c (e.code ().value ());
            if (!(e.code ().category () == generic_category () &&
                  (c == ENOSYS || // Not implemented.
                   c == EPERM)))  // Not supported by the filesystem(s).
            {
              print ("ln -s");
              fail << "unable to create symlink " << l << ": " << e;
            }

            try
            {
              mkhardlink (f, l);
              print ("ln");
            }
            catch (const system_error& e)
            {
              c = e.code ().value ();
              if (!(e.code ().category () == generic_category () &&
                  (c == ENOSYS || // Not implemented.
                   c == EPERM  || // Not supported by the filesystem(s).
                   c == EXDEV)))  // On different filesystems.
              {
                print ("ln");
                fail << "unable to create hardlink " << l << ": " << e;
              }

              try
              {
                cpfile (f, l);
                print ("cp");
              }
              catch (const system_error& e)
              {
                print ("cp");
                fail << "unable to create copy " << l << ": " << e;
              }
            }
          }

        };

        for (const windows_dll& wd: dlls)
        {
          //@@ Would be nice to avoid copying. Perhaps reuse buffers
          //   by adding path::assign() and traits::leaf().
          //
          path dp (wd.dll);     // DLL path.
          path dn (dp.leaf ()); // DLL name.

          link (dp, ad / dn);

          // Link .pdb if there is one.
          //
          if (wd.pdb != nullptr)
          {
            path pp (*wd.pdb);
            link (pp, ad / pp.leaf ());
          }

          ofs << "  <file name='" << dn.string () << "'/>\n";
        }

        ofs << "</assembly>\n";

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << am << ": " << e;
      }
    }
  }
}
