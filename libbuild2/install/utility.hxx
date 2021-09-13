// file      : libbuild2/install/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_INSTALL_UTILITY_HXX
#define LIBBUILD2_INSTALL_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>

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
          *s.var_pool ().find ("install.mode")));

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
    // are outside of this scope. If it's NULL, install all prerequisites.
    //
    inline const scope*
    install_scope (const target&)
    {
      return nullptr;
    }

    // Resolve relative installation directory path (e.g., include/libfoo) to
    // its absolute directory path (e.g., /usr/include/libfoo). If the
    // resolution encountered an unknown directory, issue diagnostics and fail
    // unless fail_unknown is false, in which case return empty directory.
    //
    // Note: implemented in rule.cxx.
    //
    LIBBUILD2_SYMEXPORT dir_path
    resolve_dir (const target&, dir_path, bool fail_unknown = true);

    LIBBUILD2_SYMEXPORT dir_path
    resolve_dir (const scope&, dir_path, bool fail_unknown = true);

    // Resolve file installation path returning empty path if not installable.
    //
    LIBBUILD2_SYMEXPORT path
    resolve_file (const file&); // rule.cxx
  }
}

#endif // LIBBUILD2_INSTALL_UTILITY_HXX
