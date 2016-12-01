#!/bin/sh

# In-tree.
#
b amalgamation/l1/ 2>/dev/null | diff --strip-trailing-cr -u test-1.out -

# Out-of-tree.
#
rm -rf a-out/
b 'configure(amalgamation/@a-out/)' 2>/dev/null

b amalgamation/l1/@a-out/l1/ 2>/dev/null | \
  diff --strip-trailing-cr -u test-2.out -

rm -rf a-out/
