#!/bin/sh

valgrind -q b -q | diff test.out -
