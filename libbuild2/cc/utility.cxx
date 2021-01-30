// file      : libbuild2/cc/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/utility.hxx>

#include <libbuild2/file.hxx>

using namespace std;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    const dir_path module_dir ("cc");
    const dir_path module_build_dir (dir_path (module_dir) /= "build");
    const dir_path module_build_modules_dir (
      dir_path (module_build_dir) /= "modules");

    void
    normalize_header (path& f)
    {
      // Interestingly, on most paltforms and with most compilers (Clang on
      // Linux being a notable exception) most system/compiler headers are
      // already normalized.
      //
      path_abnormality a (f.abnormalities ());
      if (a != path_abnormality::none)
      {
        // While we can reasonably expect this path to exit, things do go
        // south from time to time (like compiling under wine with file
        // wlantypes.h included as WlanTypes.h).
        //
        try
        {
          // If we have any parent components, then we have to verify the
          // normalized path matches realized.
          //
          path r;
          if ((a & path_abnormality::parent) == path_abnormality::parent)
          {
            r = f;
            r.realize ();
          }

          try
          {
            f.normalize ();

            // Note that we might still need to resolve symlinks in the
            // normalized path.
            //
            if (!r.empty () && f != r && path (f).realize () != r)
              f = move (r);
          }
          catch (const invalid_path&)
          {
            assert (!r.empty ()); // Shouldn't have failed if no `..`.
            f = move (r);         // Fallback to realize.
          }
        }
        catch (const invalid_path&)
        {
          fail << "invalid header path '" << f.string () << "'";
        }
        catch (const system_error& e)
        {
          fail << "invalid header path '" << f.string () << "': " << e;
        }
      }
    }
  }
}
