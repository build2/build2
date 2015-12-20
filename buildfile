# file      : buildfile
# copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = build/ tests/
./: $d doc{INSTALL LICENSE version} file{bootstrap manifest}
include $d

$src_base/doc{INSTALL}: install = false
