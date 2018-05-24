# file      : buildfile
# copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -config/ -old-tests/}                             \
    doc{INSTALL LICENSE NEWS README}                              \
    file{INSTALL.cli bootstrap* config.guess config.sub} manifest

# Don't install tests or the INSTALL file.
#
tests/:          install = false
unit-tests/:     install = false
doc{INSTALL}@./: install = false
