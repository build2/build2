# file      : buildfile
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -config/ -old-tests/ -doc/} \
    doc{NEWS README} legal{LICENSE AUTHORS} \
    file{INSTALL.cli bootstrap*}            \
    manifest

# Don't install tests or the INSTALL file.
#
tests/:          install = false
doc{INSTALL}@./: install = false
