# file      : buildfile
# copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -config/ -old-tests/}                             \
    doc{INSTALL LICENSE NEWS README}                              \
    file{INSTALL.cli bootstrap* config.guess config.sub} manifest

# Don't install tests or the INSTALL file.
#
dir{tests/}:      install = false
dir{unit-tests/}: install = false
doc{INSTALL}@./:  install = false
