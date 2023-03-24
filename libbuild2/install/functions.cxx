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
    }
  }
}
