#! /usr/bin/env bash

cur_dir="`pwd`"
trap 'cd "$cur_dir"' EXIT

export PATH=$cur_dir/../build2:$PATH

function test ()
{
  echo "testing $1"
  cd "$cur_dir/$1"
  ./test.sh
}

test "amalgam/unnamed"
test "escaping"
test "eval"
test "function/call"
test "if-else"
test "keyword"
test "names"
test "pairs"
test "quote"
test "scope"
test "target/out-qualified"
test "variable/dir-path"
test "variable/expansion"
test "variable/null"
test "variable/override"
test "variable/prepend"
test "variable/qualified"
test "variable/representation"
test "variable/type"
test "variable/type-pattern-append"
