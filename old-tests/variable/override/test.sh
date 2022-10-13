#! /usr/bin/env bash

verbose=n

# By default when MSYS2 executable (bash.exe in particular) runs another
# executable it converts arguments that look like POSIX paths to Windows
# representations. More about it at:
#
# http://www.mingw.org/wiki/Posix_path_conversion
#
# So when you run b /v=X, build2 gets 'C:/msys64/v=X' argument instead of
# '/v=X'. To disable this behavior set MSYS2_ARG_CONV_EXCL environment
# variable, so all arguments starting with / will not be converted. You can
# list more prefixes using ';' as a separator.
#
export MSYS2_ARG_CONV_EXCL=/

tmp_file=`mktemp`

# Remove temporary file on exit. Cover the case when exit due to an error.
#
trap 'rm -f $tmp_file' EXIT

function error () { echo "$*" 1>&2; exit 1; }

function fail ()
{
  if [ "$verbose" = "y" ]; then
    b $*
  else
    b -q $* 2>/dev/null
  fi

  if [ $? -eq 0 ]; then
    error "succeeded: b $*"
  fi

  return 0
}

function test ()
{
  b -q $* >$tmp_file

  if [ $? -ne 0 ]; then
    error "failed: b -q $* >$tmp_file"
  fi

  diff --strip-trailing-cr -u - $tmp_file

  if [ $? -ne 0 ]; then
    error "failed: b $*"
  fi
}

fail "p.foo= [string] bar"    # error: typed override of variable p.foo
#fail "!p.foo=bar" "!p.foo=BAR" # error: multiple global overrides of variable p.foo
#fail "p.foo=bar"  "p.foo=BAR"  # error: multiple project overrides of variable p.foo
#fail "%p.foo=bar" "%p.foo=BAR" # error: multiple project overrides of variable p.foo

test --buildfile simple p.foo=bar ./ ./ <<< "bar"  # Multiple bootstraps of the same project.

# Visibility/qualification.
#
test !p.v=X <<EOF
/     : X
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test p.v=X <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test ./p.v=X <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test .../p.v=X <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test ./p/p.v=X <<EOF
/     :
.     :
d     :
d/t   :
p     : X
p/d   : X
p/d/t : X
EOF

test .../p/p.v=X <<EOF
/     :
.     :
d     :
d/t   :
p     : X
p/d   : X
p/d/t : X
EOF

test p.v=X --buildfile loader ./p/ <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test .../p.v=X --buildfile loader ./p/ <<EOF
/     :
.     :
d     :
d/t   :
p     : X
p/d   : X
p/d/t : X
EOF

test /p.v=X <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test p.v=X p.p_a=as <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test %p.v=X <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test %p.v=X p.p_a=as <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : x
p/d   : x
p/d/t : x
EOF

test /p.v=X p.d_a=as p.p_d_a=as <<EOF
/     :
.     : X
d     : x
d/t   : x
p     : X
p/d   : x
p/d/t : x
EOF

test %p.v+=S %p.v=+P p.a=as <<EOF
/     :
.     : P x S
d     : P x S
d/t   : P x S
p     : P S
p/d   : P S
p/d/t : P S
EOF

test %p.v+=S %p.v=+P p.a=as p.p_a=as <<EOF
/     :
.     : P x S
d     : P x S
d/t   : P x S
p     : x
p/d   : x
p/d/t : x
EOF

# Append/Prepend in override.
#
test p.v+=S <<EOF
/     :
.     : S
d     : S
d/t   : S
p     : S
p/d   : S
p/d/t : S
EOF

test p.v+=S p.a=as <<EOF
/     :
.     : x S
d     : x S
d/t   : x S
p     : S
p/d   : S
p/d/t : S
EOF

test %p.v=+P p.a=as p.p_a=as <<EOF
/     :
.     : P x
d     : P x
d/t   : P x
p     : x
p/d   : x
p/d/t : x
EOF

test %p.v+=S p.v=+P p.a=as p.p_a=as <<EOF
/     :
.     : P x S
d     : P x S
d/t   : P x S
p     : P x
p/d   : P x
p/d/t : P x
EOF

# Append/Prepend in both.
#
test p.v=X p.a=ap p.d_a=ap p.p_a=ap p.p_d_a=ap <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : X
p/d   : X
p/d/t : X
EOF

test p.v+=S p.v=+P p.a=as p.d_a=ap p.d_t_a=ap p.p_a=ap p.p_d_a=ap p.p_d_t_a=ap <<EOF
/     :
.     : P x S
d     : P x s S
d/t   : P x s s S
p     : P s S
p/d   : P s s S
p/d/t : P s s s S
EOF

# These ones are surprising. I guess the moral is we shouldn't do "blind"
# cross-project append/prepend.
#
test %p.v=X p.a=as p.d_a=ap p.p_a=ap p.p_d_a=ap <<EOF
/     :
.     : X
d     : X
d/t   : X
p     : s
p/d   : s s
p/d/t : s s
EOF

test %p.v+=S p.a=as p.d_a=ap p.p_a=ap p.p_d_a=ap <<EOF
/     :
.     : x S
d     : x s S
d/t   : x s S
p     : s
p/d   : s s
p/d/t : s s
EOF

test %p.v+=S p.a=as p.d_a=ap p.p_a=ap p.p_d_a=ap ./ p/ <<EOF
/     :
.     : x S
d     : x s S
d/t   : x s S
p     : s S
p/d   : s s S
p/d/t : s s S
EOF

# Typed override.
#
test p.v+=S p.v=+P p.t=string <<EOF
/     :
.     : PS
d     : PS
d/t   : PS
p     : PS
p/d   : PS
p/d/t : PS
EOF

test p.v+=S p.v=+P p.t=string p.a=as p.d_a=ap p.d_t_a=ap p.p_a=ap p.p_d_a=ap p.p_d_t_a=ap <<EOF
/     :
.     : PxS
d     : PxsS
d/t   : PxssS
p     : PsS
p/d   : PssS
p/d/t : PsssS
EOF
