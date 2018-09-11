#! /usr/bin/env bash

version=0.8.0
date="$(date +"%B %Y")"

trap 'exit 1' ERR
set -o errtrace # Trap in functions.

function info () { echo "$*" 1>&2; }
function error () { info "$*"; exit 1; }

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

  cli -I .. -v project="build2" -v version="$version" -v date="$date" \
--include-base-last "${o[@]}" --generate-html --html-prologue-file \
man-prologue.xhtml --html-epilogue-file man-epilogue.xhtml --html-suffix \
.xhtml ../build2/$n.cli

  cli -I .. -v project="build2" -v version="$version" -v date="$date" \
--include-base-last "${o[@]}" --generate-man --man-prologue-file \
man-prologue.1 --man-epilogue-file man-epilogue.1 --man-suffix .1 \
../build2/$n.cli
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

function compile_doc () # <file> <prefix> <suffix>
{
  cli -I .. \
-v version="$(echo "$version" | sed -e 's/^\([^.]*\.[^.]*\).*/\1/')" \
-v date="$date" \
--generate-html --html-suffix .xhtml \
--html-prologue-file doc-prologue.xhtml \
--html-epilogue-file doc-epilogue.xhtml \
--link-regex '%intro(#.+)?%../../build2-toolchain/doc/build2-toolchain-intro.xhtml$1%' \
--link-regex '%bpkg([-.].+)%../../bpkg/doc/bpkg$1%' \
--link-regex '%bpkg(#.+)?%../../bpkg/doc/build2-package-manager-manual.xhtml$1%' \
--link-regex '%bdep([-.].+)%../../bdep/doc/bdep$1%' \
--link-regex '%testscript(#.+)?%build2-testscript-manual.xhtml$1%' \
--output-prefix "$2" \
--output-suffix "$3" \
"$1"

  local n="$2$(basename -s .cli $1)$3"

  html2ps -f doc.html2ps:a4.html2ps -o "$n-a4.ps" "$n.xhtml"
  ps2pdf14 -sPAPERSIZE=a4 -dOptimize=true -dEmbedAllFonts=true "$n-a4.ps" "$n-a4.pdf"

  html2ps -f doc.html2ps:letter.html2ps -o "$n-letter.ps" "$n.xhtml"
  ps2pdf14 -sPAPERSIZE=letter -dOptimize=true -dEmbedAllFonts=true "$n-letter.ps" "$n-letter.pdf"
}

compile_doc manual.cli 'build2-build-system-'
compile_doc testscript.cli 'build2-' '-manual'

# Generate INSTALL in ../
#
cli --generate-txt -o .. --txt-suffix "" ../INSTALL.cli
