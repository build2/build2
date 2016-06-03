#!/bin/sh

b -q | diff --strip-trailing-cr -u test.out -
