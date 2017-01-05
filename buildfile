# file      : buildfile
# copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = build2/ tests/ unit-tests/ doc/

./: $d \
doc{INSTALL LICENSE NEWS README version} \
file{bootstrap.sh bootstrap-msvc.bat bootstrap-mingw.bat} \
file{INSTALL.cli config.guess config.sub manifest}

include $d

# Don't install tests or the INSTALL file.
#
dir{tests/}:      install = false
dir{unit-tests/}: install = false
doc{INSTALL}@./:  install = false
