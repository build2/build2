# file      : buildfile
# license   : MIT; see accompanying LICENSE file

./: {*/ -doc/ -build/ -config/ -old-tests/}              \
    doc{INSTALL NEWS README} legal{LICENSE AUTHORS}      \
    file{INSTALL.cli bootstrap* config.guess config.sub} \
    manifest

# Don't install tests or the INSTALL file.
#
tests/:          install = false
doc{INSTALL}@./: install = false
