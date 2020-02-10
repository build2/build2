# file      : buildfile
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -config/ -old-tests/}                          \
    doc{INSTALL LICENSE COPYRIGHT NEWS README CONTRIBUTING.md} \
    file{INSTALL.cli bootstrap* config.guess config.sub}       \
    manifest

# Don't install tests or the INSTALL file.
#
tests/:          install = false
doc{INSTALL}@./: install = false
