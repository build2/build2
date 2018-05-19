// file      : build2/install/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_INSTALL_UTILITY_HXX
#define BUILD2_INSTALL_UTILITY_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/scope.hxx>
#include <build2/target.hxx>

namespace build2
{
  namespace install
  {
    // Set install path, mode for a target type.
    //
    inline void
    install_path (scope& s, const target_type& tt, dir_path d)
    {
      auto r (
        s.target_vars[tt]["*"].insert (
          var_pool.rw (s).insert ("install")));

      if (r.second) // Already set by the user?
        r.first.get () = path_cast<path> (move (d));
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
          var_pool.rw (s).insert ("install.mode")));

      if (r.second) // Already set by the user?
        r.first.get () = move (m);
    }

    template <typename T>
    inline void
    install_mode (scope& s, string m)
    {
      return install_mode (s, T::static_type, move (m));
    }

    // Resolve relative installation directory path (e.g., include/libfoo) to
    // its absolute directory path (e.g., /usr/include/libfoo).
    //
    dir_path
    resolve_dir (const target&, dir_path); // rule.cxx

    dir_path
    resolve_dir (const scope&, dir_path); // rule.cxx

    // Resolve file installation path returning empty path if not installable.
    //
    path
    resolve_file (const file&); // rule.cxx
  }
}

#endif // BUILD2_INSTALL_UTILITY_HXX
