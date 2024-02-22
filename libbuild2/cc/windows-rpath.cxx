// file      : libbuild2/cc/windows-rpath.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cerrno> // E*

#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>

#include <libbuild2/cc/link-rule.hxx>

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
    // Note: called during the execute phase.
    //
    timestamp link_rule::
    windows_rpath_timestamp (const file& t,
                             const scope& bs,
                             action a,
                             linfo li) const
    {
      timestamp r (timestamp_nonexistent);

      // Duplicate suppression similar to rpath_libraries().
      //
      rpathed_libraries ls;

      // We need to collect all the DLLs, so go into implementation of both
      // shared and static (in case they depend on shared).
      //
      auto imp = [] (const target&, bool) {return true;};

      auto lib = [&r, &ls] (
        const target* const* lc,
        const small_vector<reference_wrapper<const string>, 2>& ns,
        lflags,
        const string*,
        bool sys)
      {
        const file* l (lc != nullptr ? &(*lc)->as<file> () : nullptr);

        // We don't rpath system libraries.
        //
        if (sys)
          return false;

        if (l != nullptr)
        {
          // Suppress duplicates.
          //
          if (find (ls.begin (), ls.end (), l) != ls.end ())
            return false;

          // Ignore static libraries. Note that this can be an "undiscovered"
          // DLL (see search_library()).
          //
          if (l->is_a<libs> () && !l->path ().empty ()) // Also covers binless.
          {
            // Handle the case where the library is a member of a group (for
            // example, people are trying to hack something up with pre-built
            // libraries; see GH issue #366).
            //
            timestamp t;
            if (l->group_state (action () /* inner */))
            {
              t = l->group->is_a<mtime_target> ()->mtime ();
              assert (t != timestamp_unknown);
            }
            else
              t = l->load_mtime ();

            if (t > r)
              r = t;
          }

          ls.push_back (l);
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
          for (const string& f: ns)
          {
            size_t p (path::traits_type::find_extension (f));

            if (p != string::npos && icasecmp (f.c_str () + p + 1, "dll") == 0)
            {
              timestamp t (mtime (f.c_str ()));

              if (t > r)
                r = t;
            }
          }
        }

        return true;
      };

      library_cache lib_cache;
      for (const prerequisite_target& pt: t.prerequisite_targets[a])
      {
        // Note: during execute so check for ad hoc first to avoid data races.
        //
        if (pt.adhoc () || pt == nullptr)
          continue;

        bool la;
        const file* f;

        if ((la = (f = pt->is_a<liba>  ())) ||
            (la = (f = pt->is_a<libux> ())) || // See through.
            (     f = pt->is_a<libs>  ()))
          process_libraries (a, bs, li, sys_lib_dirs,
                             *f, la, pt.data,
                             imp, lib, nullptr,
                             true /* self */,
                             false /* proc_opt_group */,
                             &lib_cache);
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
      // Note that we cannot reuse windows_dlls for duplicate suppression
      // since it would only keep track of shared libraries.
      //
      windows_dlls r;
      rpathed_libraries ls;

      struct
      {
        const scope&       bs;
        rpathed_libraries& ls;
      } d {bs, ls};

      auto imp = [] (const target&, bool) {return true;};

      auto lib = [&d, &r] (
        const target* const* lc,
        const small_vector<reference_wrapper<const string>, 2>& ns,
        lflags,
        const string*,
        bool sys)
      {
        const file* l (lc != nullptr ? &(*lc)->as<file> () : nullptr);

        if (sys)
          return false;

        if (l != nullptr)
        {
          // Suppress duplicates.
          //
          if (find (d.ls.begin (), d.ls.end (), l) != d.ls.end ())
            return false;

          if (l->is_a<libs> () && !l->path ().empty ()) // Also covers binless.
          {
            // Get .pdb if there is one.
            //
            const target_type* tt (d.bs.find_target_type ("pdb"));
            const target* pdb (tt != nullptr
                               ? find_adhoc_member (*l, *tt)
                               : nullptr);

            // Here we assume it's not a duplicate due to the check above.
            //
            r.push_back (
              windows_dll {
                ns[0],
                pdb != nullptr ? pdb->as<file> ().path ().string () : string (),
              });
          }

          d.ls.push_back (l);
        }
        else
        {
          string pdb;
          for (const string& f: ns)
          {
            size_t p (path::traits_type::find_extension (f));

            if (p != string::npos && icasecmp (f.c_str () + p + 1, "dll") == 0)
            {
              if (find_if (r.begin (), r.end (),
                           [&f] (const windows_dll& e)
                           {
                             return e.dll.get () == f;
                           }) == r.end ())
              {
                // See if we can find a corresponding .pdb. First try "our"
                // naming: foo.dll.pdb.
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

                r.push_back (
                  windows_dll {f, pdb.empty () ? string () : move (pdb)});
              }
            }
          }
        }

        return true;
      };

      library_cache lib_cache;
      for (const prerequisite_target& pt: t.prerequisite_targets[a])
      {
        // Note: during execute so check for ad hoc first to avoid data races.
        //
        if (pt.adhoc () || pt == nullptr)
          continue;

        bool la;
        const file* f;

        if ((la = (f = pt->is_a<liba>  ())) ||
            (la = (f = pt->is_a<libux> ())) || // See through.
            (      f = pt->is_a<libs>  ()))
          process_libraries (a, bs, li, sys_lib_dirs,
                             *f, la, pt.data,
                             imp, lib, nullptr,
                             true /* self */,
                             false /* proc_opt_group */,
                             &lib_cache);
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
        if (ts != timestamp_nonexistent && ts <= mtime (am))
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
        rmdir_status s (rmdir_r (t.ctx, ad, empty, 3));

        if (empty)
          return;

        if (s == rmdir_status::not_exist)
          mkdir (ad, 3);
      }

      // Symlink or copy the DLLs.
      //
      {
        const scope& as (t.weak_scope ()); // Amalgamation.

        auto link = [&as] (const path& f, const path& l)
        {
          auto print = [&f, &l] (const char* cmd)
          {
            if (verb >= 3)
              text << cmd << ' ' << f << ' ' << l;
          };

          // First we try to create a symlink. If that fails (e.g., "Windows
          // happens"), then we resort to hard links. If that doesn't work
          // out either (e.g., not on the same filesystem), then we fall back
          // to copies.
          //
          // For the symlink use a relative target path if both paths are part
          // of the same amalgamation. This way if the amalgamation is moved
          // as a whole, the links will remain valid.
          //
          // Note: mkanylink() is from libbutl and thus doesn't handle the
          //       dry-run mode.
          //
          try
          {
            switch (as.ctx.dry_run
                    ? entry_type::symlink
                    : mkanylink (f, l,
                                 true                   /* copy */,
                                 f.sub (as.out_path ()) /* relative */))
            {
            case entry_type::regular: print ("cp");    break;
            case entry_type::symlink: print ("ln -s"); break;
            case entry_type::other:   print ("ln");    break;
            default:                  assert (false);
            }
          }
          catch (const pair<entry_type, system_error>& e)
          {
            const char* w (nullptr);
            switch (e.first)
            {
            case entry_type::regular: print ("cp");    w = "copy";     break;
            case entry_type::symlink: print ("ln -s"); w = "symlink";  break;
            case entry_type::other:   print ("ln");    w = "hardlink"; break;
            default:                  assert (false);
            }

            fail << "unable to make " << w << ' ' << l << ": " << e.second;
          }
        };

        for (const windows_dll& wd: dlls)
        {
          //@@ Would be nice to avoid copying. Perhaps reuse buffers
          //   by adding path::assign() and traits::leaf().
          //
          path dp (wd.dll.get ()); // DLL path.
          path dn (dp.leaf ());    // DLL name.

          link (dp, ad / dn);

          // Link .pdb if there is one.
          //
          if (!wd.pdb.empty ())
          {
            path pp (wd.pdb);
            link (pp, ad / pp.leaf ());
          }
        }
      }

      if (verb >= 3)
        text << "cat >" << am;

      if (t.ctx.dry_run)
        return;

      auto_rmfile rm (am);

      try
      {
        ofdstream os (am);

        const char* pa (windows_manifest_arch (tcpu));

        os << "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
           << "<assembly xmlns='urn:schemas-microsoft-com:asm.v1'\n"
           << "          manifestVersion='1.0'>\n"
           << "  <assemblyIdentity name='" << an << "'\n"
           << "                    type='win32'\n"
           << "                    processorArchitecture='" << pa << "'\n"
           << "                    version='0.0.0.0'/>\n";



        for (const windows_dll& wd: dlls)
          os << "  <file name='" << path (wd.dll).leaf () << "'/>\n";

        os << "</assembly>\n";

        os.close ();
        rm.cancel ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << am << ": " << e;
      }
    }
  }
}
