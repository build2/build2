// file      : libbuild2/version/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/version/utility.hxx>

#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbuild2/context.hxx>
#include <libbuild2/filesystem.hxx>  // path_perms()
#include <libbuild2/diagnostics.hxx>

using namespace butl;

namespace build2
{
  namespace version
  {
    auto_rmfile
    fixup_manifest (context& ctx,
                    const path& in,
                    path out,
                    const standard_version& v)
    {
      auto_rmfile r (move (out), !ctx.dry_run /* active */);

      if (!ctx.dry_run)
      {
        try
        {
          permissions perm (path_perms (in));

          ifdstream ifs (in);
          manifest_parser p (ifs, in.string ());

          auto_fd ofd (fdopen (r.path,
                               fdopen_mode::out       |
                               fdopen_mode::create    |
                               fdopen_mode::exclusive |
                               fdopen_mode::binary,
                               perm));

          ofdstream ofs (move (ofd));
          manifest_serializer s (ofs, r.path.string ());

          manifest_name_value nv (p.next ());
          assert (nv.name.empty () && nv.value == "1"); // We just loaded it.
          s.next (nv.name, nv.value);

          for (nv = p.next (); !nv.empty (); nv = p.next ())
          {
            if (nv.name == "version")
              nv.value = v.string ();

            s.next (nv.name, nv.value);
          }

          s.next (nv.name, nv.value); // End of manifest.
          s.next (nv.name, nv.value); // End of stream.

          ofs.close ();
          ifs.close ();
        }
        catch (const manifest_parsing& e)
        {
          location l (in, e.line, e.column);
          fail (l) << e.description;
        }
        catch (const manifest_serialization& e)
        {
          location l (r.path);
          fail (l) << e.description;
        }
        catch (const io_error& e)
        {
          fail << "io error: " << e <<
            info << "while reading " << in <<
            info << "while writing " << r.path;
        }
      }

      return r;
    }
  }
}
