#! /usr/bin/env bash

version="0.4.0"
date="April 2016"

trap 'exit 1' ERR
set -o errtrace # Trap in functions.

function info () { echo "$*" 1>&2; }
function error () { info "$*"; exit 1; }

while [ $# -gt 0 ]; do
  case $1 in
    --clean)
      rm -f b*.xhtml b*.1
      rm -f build2-build-system-manual*.ps \
	 build2-build-system-manual*.pdf   \
	 build2-build-system-manual.xhtml
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

# Manual.
#
cli -I .. -v version="$version" -v date="$date" \
--generate-html --html-suffix .xhtml \
--html-prologue-file doc-prologue.xhtml \
--html-epilogue-file doc-epilogue.xhtml \
--output-prefix build2-build-system- manual.cli

html2ps -f doc.html2ps:a4.html2ps -o build2-build-system-manual-a4.ps build2-build-system-manual.xhtml
ps2pdf14 -sPAPERSIZE=a4 -dOptimize=true -dEmbedAllFonts=true build2-build-system-manual-a4.ps build2-build-system-manual-a4.pdf

html2ps -f doc.html2ps:letter.html2ps -o build2-build-system-manual-letter.ps build2-build-system-manual.xhtml
ps2pdf14 -sPAPERSIZE=letter -dOptimize=true -dEmbedAllFonts=true build2-build-system-manual-letter.ps build2-build-system-manual-letter.pdf
