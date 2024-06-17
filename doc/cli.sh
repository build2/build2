#! /usr/bin/env bash

version=0.17.0

trap 'exit 1' ERR
set -o errtrace # Trap in functions.

function info () { echo "$*" 1>&2; }
function error () { info "$*"; exit 1; }

date="$(date +"%B %Y")"
copyright="$(sed -n -re 's%^Copyright \(c\) (.+) \(see the AUTHORS file\)\.$%\1%p' ../LICENSE)"

while [ $# -gt 0 ]; do
  case $1 in
    --clean)
      rm -f b*.xhtml b*.1
      rm -f build2-build-system-manual.xhtml
      rm -f build2-testscript-manual.xhtml
      rm -f *.ps *.pdf
      exit 0
      ;;
    *)
      error "unexpected $1"
      ;;
  esac
done

function compile ()
{
  local n=$1; shift

  # Use a bash array to handle empty arguments.
  #
  local o=()
  while [ $# -gt 0 ]; do
    o=("${o[@]}" "$1")
    shift
  done

  cli -I .. \
-v project="build2" \
-v version="$version" \
-v date="$date" \
-v copyright="$copyright" \
--include-base-last "${o[@]}" \
--generate-html --html-suffix .xhtml \
--html-prologue-file man-prologue.xhtml \
--html-epilogue-file man-epilogue.xhtml \
--link-regex '%b(#.+)?%build2-build-system-manual.xhtml$1%' \
../libbuild2/$n.cli

  cli -I .. \
-v project="build2" \
-v version="$version" \
-v date="$date" \
-v copyright="$copyright" \
--include-base-last "${o[@]}" \
--generate-man --man-suffix .1 --ascii-tree \
--man-prologue-file man-prologue.1 \
--man-epilogue-file man-epilogue.1 \
--link-regex '%b(#.+)?%$1%' \
../libbuild2/$n.cli
}

o="--output-prefix b-"

# A few special cases.
#
compile "b" $o --output-prefix "" --suppress-undocumented

pages=""

for p in $pages; do
  compile $p $o
done

# Manuals.
#

# @@ Note that we now have --ascii-tree CLI option.
#
function xhtml_to_ps () # <from> <to> [<html2ps-options>]
{
  local from="$1"
  shift
  local to="$1"
  shift

  sed -e 's/├/|/g' -e 's/│/|/g' -e 's/─/-/g' -e 's/└/\xb7/g' "$from" | \
  html2ps "${@}" -o "$to"
}

function compile_doc () # <file> <prefix> <suffix>
{
  local file="$1"
  shift
  local prefix="$1"
  shift
  local suffix="$1"
  shift

  cli -I .. \
-v version="$(echo "$version" | sed -e 's/^\([^.]*\.[^.]*\).*/\1/')" \
-v date="$date" \
-v copyright="$copyright" \
--generate-html --html-suffix .xhtml \
--html-prologue-file doc-prologue.xhtml \
--html-epilogue-file doc-epilogue.xhtml \
--link-regex '%intro(#.+)?%../../build2-toolchain/doc/build2-toolchain-intro.xhtml$1%' \
--link-regex '%bpkg([-.].+)%../../bpkg/doc/bpkg$1%' \
--link-regex '%bpkg(#.+)?%../../bpkg/doc/build2-package-manager-manual.xhtml$1%' \
--link-regex '%bdep([-.].+)%../../bdep/doc/bdep$1%' \
--link-regex '%testscript(#.+)?%build2-testscript-manual.xhtml$1%' \
--link-regex '%build2(#.+)?%build2-build-system-manual.xhtml$1%' \
--output-prefix "$prefix" \
--output-suffix "$suffix" \
"${@}" \
"$file"

  local n="$prefix$(basename -s .cli $file)$suffix"

  xhtml_to_ps "$n.xhtml" "$n-a4.ps" -f doc.html2ps:a4.html2ps
  ps2pdf14 -sPAPERSIZE=a4 -dOptimize=true -dEmbedAllFonts=true "$n-a4.ps" "$n-a4.pdf"

  xhtml_to_ps "$n.xhtml" "$n-letter.ps" -f doc.html2ps:letter.html2ps
  ps2pdf14 -sPAPERSIZE=letter -dOptimize=true -dEmbedAllFonts=true "$n-letter.ps" "$n-letter.pdf"
}

# @@ TODO: replace -I. with $out_base and get rid of backlinking once
#          migrated to reciped.
#
# Note: we have to manually map \h to h2 since we break the doc string.
#
b update: alias{functions}
compile_doc manual.cli 'build2-build-system-' '' --html-heading-map h=h2 -I .
compile_doc testscript.cli 'build2-' '-manual'

# Generate INSTALL in ../
#
cli --generate-txt -o .. --txt-suffix "" ../INSTALL.cli
