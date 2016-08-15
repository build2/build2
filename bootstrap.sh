#!/bin/sh

# file      : bootstrap.sh
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

usage="Usage: $0 [--help] [--cxx <compiler>] [--cxxflags <flags>] [<host>]"

cxx=g++
cxxflags=
libbutl=
host=

while test $# -ne 0; do
  case $1 in
    --help)
      echo "$usage" 1>&2
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
    --cxxflags)
      shift
      if test $# -eq 0; then
	echo "error: c++ compiler flags expected after --cxxflags" 1>&2
	echo "$usage" 1>&2
	exit 1
      fi
      cxxflags=$1
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
    *)
      host=$1
      shift
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

echo "using $host as build2 host" 1>&2

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

cppflags='-DBUILD2_HOST_TRIPLET="'$host'"'

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

echo $cxx -std=c++1y -I$libbutl -I. $cppflags $cxxflags -o build2/b-boot $src 1>&2
exec $cxx -std=c++1y -I$libbutl -I. $cppflags $cxxflags -o build2/b-boot $src
