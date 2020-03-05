# file      : buildfile
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -config/ -old-tests/}                    \
    doc{INSTALL LICENSE AUTHORS NEWS README}             \
    file{INSTALL.cli bootstrap* config.guess config.sub} \
    manifest

# Don't install tests or the INSTALL file.
#
tests/:          install = false
doc{INSTALL}@./: install = false
