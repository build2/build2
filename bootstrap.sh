#!/bin/sh

# file      : bootstrap.sh
# copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

usage="Usage: $0 [-h] [--libbutl <dir>] [--host <triplet>] <cxx> [<cxx-option>...]"

diag ()
{
  echo "$*" 1>&2
}

cxx=
libbutl=
host=

while test $# -ne 0; do
  case "$1" in
    -h|--help)
      diag
      diag "$usage"
      diag
      diag "The script expects to find the libbutl/ or libbutl-*/ directory either"
      diag "in the current directory (build2 root) or one level up. The result is"
      diag "saved as build2/b-boot."
      diag
      diag "Example usage:"
      diag
      diag "$0 g++"
      diag
      diag "See the INSTALL file for details."
      diag
      exit 0
      ;;
    --libbutl)
      shift
      if test $# -eq 0; then
	diag "error: libbutl path expected after --libbutl"
	diag "$usage"
	exit 1
      fi
      if test ! -d "$1"; then
	diag "error: libbutl directory '$1' does not exist"
	exit 1
      fi
      libbutl="${1%/}"
      shift
      ;;
    --host)
      shift
      if test $# -eq 0; then
	diag "error: host triplet expected after --host"
	diag "$usage"
	exit 1
      fi
      host="$1"
      shift
      ;;
    *)
      cxx="$1"
      shift
      break
      ;;
  esac
done

if test -z "$cxx"; then
  diag "error: compiler executable expected"
  diag "$usage"
  exit 1
fi

if test -z "$host"; then
  if ! host="$(./config.guess)"; then
    diag "error: unable to guess host triplet"
    exit 1
  fi
else
  if ! chost="$(./config.sub "$host")"; then
    diag "error: unable to canonicalize host triplet '$host'"
    exit 1
  fi
  host="$chost"
fi

# See if there is libbutl or libbutl-* in the current directory and
# one directory up.
#
if test -z "$libbutl"; then
  if test -d libbutl; then
    libbutl="libbutl"
  else
    libbutl="$(echo libbutl-*/)"
    libbutl="${libbutl%/}"
    if test ! -d "$libbutl"; then
      libbutl=
    fi
  fi
fi

if test -z "$libbutl"; then
  if test -d ../libbutl; then
    libbutl="../libbutl"
  else
    libbutl="$(echo ../libbutl-*/)"
    libbutl="${libbutl%/}"
    if test ! -d "$libbutl"; then
      libbutl=
    fi
  fi
fi

if test -z "$libbutl"; then
  diag "error: unable to find libbutl, use --libbutl to specify its location"
  exit 1
fi

src="build2/*.cxx"
src="$src build2/config/*.cxx"
src="$src build2/dist/*.cxx"
src="$src build2/bin/*.cxx"
src="$src build2/c/*.cxx"
src="$src build2/cc/*.cxx"
src="$src build2/cxx/*.cxx"
src="$src build2/cli/*.cxx"
src="$src build2/test/*.cxx"
src="$src build2/test/script/*.cxx"
src="$src build2/version/*.cxx"
src="$src build2/install/*.cxx"
src="$src $libbutl/libbutl/*.cxx"

# Note that for as long as we support GCC 4.9 we have to compile in the C++14
# mode since 4.9 doesn't recognize c++1z.
#
set -x
"$cxx" "-I$libbutl" -I. -DBUILD2_BOOTSTRAP '-DBUILD2_HOST_TRIPLET="'"$host"'"' -std=c++1y "$@" -o build2/b-boot $src -lpthread
