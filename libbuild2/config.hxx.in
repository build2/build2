// file      : libbuild2/config.hxx.in -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

// This file is included by <libbuild2/types.hxx> so normally you don't need
// to include it directly. Note that this file is included unprocessed (i.e.,
// as an .in) during bootstrap.
//
// Also, note that some BUILD_* configuration macros are passed directly from
// the buildfile with the -D options.

#ifndef LIBBUILD2_CONFIG_HXX
#define LIBBUILD2_CONFIG_HXX

// Currently the value is adjusted manually during release but in the future
// the idea is to use version metadata (e.g., 1.2.3-a.1+0.stage). This way it
// will all be managed in a central place (manifest), we can teach the version
// module to extract it, and we can also set it for the other packages in the
// toolchain. Bootstrap will be a problem though. (Maybe set it to nullptr and
// say that it shall not be queried?)
//
#define LIBBUILD2_STAGE false

// Modification time sanity checks are by default only enabled for the staged
// version unless we are on Windows (which is known not to guarantee
// monotonically increasing mtimes). But this can be overridden at runtime
// with --[no-]mtime-check.
//
#if LIBBUILD2_STAGE || defined(_WIN32)
#  define LIBBUILD2_MTIME_CHECK true
#else
#  define LIBBUILD2_MTIME_CHECK false
#endif

#ifdef BUILD2_BOOTSTRAP
#else
#endif

#endif // LIBBUILD2_CONFIG_HXX
