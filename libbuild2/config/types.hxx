// file      : libbuild2/config/types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONFIG_TYPES_HXX
#define LIBBUILD2_CONFIG_TYPES_HXX

#include <libbuild2/types.hxx>

namespace build2
{
  namespace config
  {
    // The origin of the value of a configuration variable.
    //
    enum class variable_origin
    {
      undefined,  // Undefined.
      default_,   // Default value from the config directive.
      buildfile,  // Value from a buildfile, normally config.build.
      override_   // Value from a command line override.
    };
  }
}

#endif // LIBBUILD2_CONFIG_TYPES_HXX
