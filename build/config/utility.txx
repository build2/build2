// file      : build/config/utility.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <utility> // move()

#include <build/scope>
#include <build/variable>

namespace build
{
  namespace config
  {
    template <typename T>
    std::pair<const T&, bool>
    required (scope& root, const char* name, const T& def_value)
    {
      T r;
      const variable& var (variable_pool.find (name));

      if (auto v = root[var])
      {
        const T& s (v.as<const T&> ());

        if (!v.belongs (*global_scope)) // A value from (some) config.build.
          return std::pair<const T&, bool> (s, false);

        r = s;
      }
      else
        r = def_value;

      auto v (root.assign (var));
      v = std::move (r);

      return std::pair<const T&, bool> (v.as<const T&> (), true);
    }

    template <typename T>
    const T*
    optional (scope& root, const char* name)
    {
      const T* r (nullptr);
      const variable& var (variable_pool.find (name));

      auto v (root[var]);

      if (v.defined ())
      {
        if (v.belongs (*global_scope))
          root.assign (var) = v;

        r =  v.null () ? nullptr : &v.as<const T&> ();
      }
      else
        root.assign (var) = nullptr;

      return r;
    }
  }
}
