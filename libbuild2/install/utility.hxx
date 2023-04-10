// file      : libbuild2/install/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_INSTALL_UTILITY_HXX
#define LIBBUILD2_INSTALL_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/filesystem.hxx> // entry_type

#include <libbuild2/export.hxx>

namespace build2
{
  namespace install
  {
    // Set install path, mode for a target type.
    //
    // Note: should only be called if the install module is loaded.
    //
    inline void
    install_path (scope& s, const target_type& tt, dir_path d)
    {
      auto r (
        s.target_vars[tt]["*"].insert (
          *s.var_pool ().find ("install")));

      if (r.second) // Already set by the user?
        r.first = path_cast<path> (move (d));
    }

    template <typename T>
    inline void
    install_path (scope& s, dir_path d)
    {
      return install_path (s, T::static_type, move (d));
    }

    inline void
    install_mode (scope& s, const target_type& tt, string m)
    {
      auto r (
        s.target_vars[tt]["*"].insert (
          *s.ctx.var_pool.find ("install.mode")));

      if (r.second) // Already set by the user?
        r.first = move (m);
    }

    template <typename T>
    inline void
    install_mode (scope& s, string m)
    {
      return install_mode (s, T::static_type, move (m));
    }

    // Return the "installation scope". We do not install prerequisites (for
    // example, shared libraries) of targets (for example, executables) that
    // belong to projects outside of this scope. If it's NULL, install
    // prerequisites from all projects. See also config.install.scope.
    //
    // Note that this should not apply to update-for-install. Failed that we
    // may end up using incompatibly-built prerequisites (e.g., a library) in
    // a target built for install (e.g., an executable).
    //
    LIBBUILD2_SYMEXPORT const scope*
    install_scope (const target&);

    // Resolve relative installation directory path (e.g., include/libfoo) to
    // its absolute and normalized directory path (e.g., /usr/include/libfoo).
    // If the resolution encountered an unknown directory, issue diagnostics
    // and fail unless fail_unknown is false, in which case return empty
    // directory.
    //
    // For rel_base semantics, see the $install.resolve() documentation. Note
    // that fail_unknown does not apply to the rel_base resolution.
    //
    // Note: implemented in rule.cxx.
    //
    LIBBUILD2_SYMEXPORT dir_path
    resolve_dir (const target&,
                 dir_path,
                 dir_path rel_base = {},
                 bool fail_unknown = true);

    LIBBUILD2_SYMEXPORT dir_path
    resolve_dir (const scope&,
                 dir_path,
                 dir_path rel_base = {},
                 bool fail_unknown = true);

    // Resolve file installation path returning empty path if not installable.
    //
    LIBBUILD2_SYMEXPORT path
    resolve_file (const file&); // rule.cxx

    // Given an abolute path return its chroot'ed version, if any, accoring to
    // install.chroot.
    //
    template <typename P>
    inline P
    chroot_path (const scope& rs, const P& p)
    {
      assert (p.absolute ());
      const dir_path* d (cast_null<dir_path> (rs["install.chroot"]));
      return d != nullptr ? *d / p.leaf (p.root_directory ()) : p;
    }

    // Installation filtering (config.install.filter).
    //
    // If entry type is a directory, then leaf must be empty.
    //
    using filters = vector<pair<string, optional<string>>>;

    LIBBUILD2_SYMEXPORT bool
    filter_entry (const scope& rs,
                  const dir_path& base,
                  const path& leaf,
                  entry_type);
  }
}

#endif // LIBBUILD2_INSTALL_UTILITY_HXX
