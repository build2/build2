#!/bin/sh

valgrind -q b -q | diff -u test.out -
