# file      : buildfile
# copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = build/ tests/
./: $d doc{INSTALL LICENSE} file{bootstrap version}
include $d

doc{INSTALL}: install = false
