#! /usr/bin/env bash

cur_dir="`pwd`"
trap 'cd "$cur_dir"' EXIT

export PATH=$cur_dir/../build2:$PATH

function test ()
{
  echo "Testing $1"
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
test "variable/expansion"
test "variable/null"
test "variable/override"
test "variable/prepend"
test "variable/qualified"
test "variable/type"
