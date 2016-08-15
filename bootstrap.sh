#!/bin/sh

# file      : bootstrap.sh
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

usage="Usage: $0 [-h] [--cxx <file>] [--libbutl <dir>] [--host <triplet>] [<options>]"

cxx=g++
libbutl=
host=

while test $# -ne 0; do
  case $1 in
    -h|--help)
      echo "$usage" 1>&2
      echo 1>&2
      echo "The script expects to find the libbutl/ or libbutl-*/ directory either" 1>&2
      echo "in the current directory (build2 root) or one level up." 1>&2
      echo 1>&2
      echo "See the INSTALL file for details." 1>&2
      exit 0
      ;;
    --cxx)
      shift
      if test $# -eq 0; then
	echo "error: c++ compiler executable expected after --cxx" 1>&2
	echo "$usage" 1>&2
	exit 1
      fi
      cxx=$1
      shift
      ;;
    --libbutl)
      shift
      if test $# -eq 0; then
	echo "error: libbutl path expected after --libbutl" 1>&2
	echo "$usage" 1>&2
	exit 1
      fi
      if test ! -d "$1"; then
	echo "error: libbutl directory '$1' does not exist" 1>&2
	exit 1
      fi
      libbutl=$1
      shift
      ;;
    --host)
      shift
      if test $# -eq 0; then
	echo "error: host triplet expected after --host" 1>&2
	echo "$usage" 1>&2
	exit 1
      fi
      host=$1
      shift
      ;;
    *)
      break
      ;;
  esac
done

if test -z "$host"; then
  if ! host=`./config.guess`; then
    echo "error: unable to guess host triplet" 1>&2
    exit 1
  fi
else
  if ! chost=`./config.sub $host`; then
    echo "error: unable to canonicalize host triplet '$host'" 1>&2
    exit 1
  fi
  host=$chost
fi

# See if there is libbutl or libbutl-* in the current directory and
# one directory up.
#
if test -z "$libbutl"; then
  if test -d libbutl; then
    libbutl=libbutl
  else
    libbutl=`echo libbutl-*/`
    if test ! -d "$libbutl"; then
      libbutl=
    fi
  fi
fi

if test -z "$libbutl"; then
  if test -d ../libbutl; then
    libbutl=../libbutl
  else
    libbutl=`echo ../libbutl-*/`
    if test ! -d "$libbutl"; then
      libbutl=
    fi
  fi
fi

if test -z "$libbutl"; then
  echo "error: unable to find libbutl, use --libbutl to specify its location" 1>&2
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
src="$src build2/install/*.cxx"
src="$src $libbutl/butl/*.cxx"

echo $cxx -I$libbutl -I. '-DBUILD2_HOST_TRIPLET="'$host'"' -std=c++1y $* -o build2/b-boot $src
exec $cxx -I$libbutl -I. '-DBUILD2_HOST_TRIPLET="'$host'"' -std=c++1y $* -o build2/b-boot $src
