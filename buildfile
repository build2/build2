# file      : buildfile
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = build2/ tests/ doc/

./: $d \
doc{INSTALL LICENSE version} \
file{INSTALL.cli config.guess config.sub bootstrap manifest}

include $d

doc{INSTALL*}: install = false
