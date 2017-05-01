// file      : build2/version.hxx.in -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_VERSION // Note: using the version macro itself.

// Note: using build2 standard versioning scheme. The numeric version format
// is AAABBBCCCDDDE where:
//
// AAA - major version number
// BBB - minor version number
// CCC - bugfix version number
// DDD - alpha / beta (DDD + 500) version number
// E   - final (0) / snapshot (1)
//
// When DDDE is not 0, 1 is subtracted from AAABBBCCC. For example:
//
// Version      AAABBBCCCDDDE
//
// 0.1.0        0000010000000
// 0.1.2        0000010010000
// 1.2.3        0010020030000
// 2.2.0-a.1    0020019990010
// 3.0.0-b.2    0029999995020
// 2.2.0-a.1.z  0020019990011
//
#define BUILD2_VERSION       49995001ULL
#define BUILD2_VERSION_STR   "0.5.0-b.0.z"
#define BUILD2_VERSION_ID    "0.5.0-b.0.z"

#define BUILD2_VERSION_MAJOR 0
#define BUILD2_VERSION_MINOR 5
#define BUILD2_VERSION_PATCH 0

#define BUILD2_PRE_RELEASE   true

#define BUILD2_SNAPSHOT      18446744073709551615ULL
#define BUILD2_SNAPSHOT_ID   ""

#include <libbutl/version.hxx>

#ifdef LIBBUTL_VERSION
#  if !((LIBBUTL_VERSION > 49995001ULL || (LIBBUTL_VERSION == 49995001ULL && LIBBUTL_SNAPSHOT >= 1ULL)) && LIBBUTL_VERSION < 49995010ULL)
#    error incompatible libbutl version, libbutl [0.5.0-b.0.1 0.5.0-b.1) is required
#  endif
#endif

#endif // BUILD2_VERSION
