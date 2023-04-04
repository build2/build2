// file      : libbuild2/install/functions.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/install/utility.hxx>

namespace build2
{
  namespace install
  {
    void
    functions (function_map& m)
    {
      function_family f (m, "install");

      // $install.resolve(<dir>[, <rel_base>])
      //
      // @@ TODO: add overload to call resolve_file().
      //
      // Resolve potentially relative install.* value to an absolute and
      // normalized directory based on (other) install.* values visible from
      // the calling scope.
      //
      // If rel_base is specified and is not empty, then make the resulting
      // directory relative to it. If rel_base itself is relative, first
      // resolve it to an absolute and normalized directory based on install.*
      // values. Note that this argument is mandatory if this function is
      // called during relocatable installation (install.relocatable is true).
      // While you can pass empty directory to suppress this functionality,
      // make sure this does not render the result non-relocatable.
      //
      // As an example, consider an executable that supports loading plugins
      // and requires the plugin installation directory to be embedded into
      // the executable during the build. The common way to support
      // relocatable installations for such cases is to embed a path relative
      // to the executable and complete it at runtime. If you would like to
      // always use the relative path, regardless of whether the installation
      // is relocatable of not, then you can simply always pass rel_base, for
      // example:
      //
      // plugin_dir = $install.resolve($install.lib, $install.bin)
      //
      // Alternatively, if you would like to continue using absolute paths for
      // non-relocatable installations, then you can use something like this:
      //
      // plugin_dir = $install.resolve($install.lib, ($install.relocatable ? $install.bin : [dir_path] ))
      //
      // Finally, if you are unable to support relocatable installations, the
      // correct way to handle this is NOT to always pass an empty path for
      // rel_base but rather assert in root.build that your project does not
      // support relocatable installations, for example:
      //
      // assert (!$install.relocatable) 'relocatable installation not supported'
      //
      // Note that this function is not pure.
      //
      f.insert (".resolve", false) += [] (const scope* s,
                                          dir_path dir,
                                          optional<dir_path> rel_base)
      {
        if (s == nullptr)
          fail << "install.resolve() called out of scope" << endf;

        if (!rel_base)
        {
          const scope& rs (*s->root_scope ());

          if (cast_false<bool> (rs["install.relocatable"]))
          {
            fail << "relocatable installation requires relative base "
                 << "directory" <<
              info << "pass empty relative base directory if this call does "
                 << "not affect installation relocatability" <<
              info << "or add `assert (!$install.relocatable) 'relocatable "
                 << "installation not supported'` before the call";
          }
        }

        return resolve_dir (*s,
                            move (dir),
                            rel_base ? move (*rel_base) : dir_path ());
      };

      // @@ TODO: add $install.chroot().

      // $install.filter(<path>[, <type>])
      //
      // Apply filters from config.install.filter and return true if the
      // specified filesystem entry should be installed/uninstalled. Note that
      // the entry is specified as an absolute and normalized installation
      // path (so not $path($>) but $install.resolve($>)).
      //
      // The type argument can be one of `regular`, `directory`, or `symlink`.
      // If unspecified, either `directory` or `regular` is assumed, based on
      // whether path is syntactially a directory (ends with a directory
      // separator).
      //
      // Note that this function is not pure.
      //
      f.insert (".filter", false) += [] (const scope* s,
                                         path p,
                                         optional<names> ot)
      {
        if (s == nullptr)
          fail << "install.filter() called out of scope" << endf;

        entry_type t;
        if (ot)
        {
          string v (convert<string> (move (*ot)));

          if      (v == "regular")   t = entry_type::regular;
          else if (v == "directory") t = entry_type::directory;
          else if (v == "symlink")   t = entry_type::symlink;
          else throw invalid_argument ("unknown type '" + v + '\'');
        }
        else
          t = p.to_directory () ? entry_type::directory : entry_type::regular;

        // Split into directory and leaf.
        //
        dir_path d;
        if (t == entry_type::directory)
        {
          d = path_cast<dir_path> (move (p));
          p = path (); // No leaf.
        }
        else
        {
          d = p.directory ();
          p.make_leaf ();
        }

        return filter_entry (*s->root_scope (), d, p, t);
      };
    }
  }
}
