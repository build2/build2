// file      : tests/build/parser/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <sstream>
#include <iostream>

#include <build/path>
#include <build/lexer>
#include <build/parser>

using namespace std;
using namespace build;

static bool
parse (const char*);

int
main ()
{
  assert (parse (""));
  assert (parse ("foo:"));
  assert (parse ("foo bar:"));
  assert (parse ("foo:\nbar:"));
  assert (parse ("foo: bar"));
  assert (parse ("foo: bar baz"));
  assert (parse ("foo bar: baz biz"));

  assert (parse ("{foo}:"));
  assert (parse ("{foo bar}:"));
  assert (parse ("{{foo bar}}:"));
  assert (parse ("{{foo bar} {baz} {biz fox} fix}:"));

  assert (parse ("exe{foo}:"));
  assert (parse ("exe{foo bar}:"));
  assert (parse ("{exe{foo bar}}:"));
  assert (parse ("exe{{foo bar} fox}:"));
  assert (parse ("exe{foo}: obj{bar baz} biz.o lib{fox}"));

  assert (!parse (":"));
  assert (!parse ("foo"));
  assert (!parse ("{"));
  assert (!parse ("{foo:"));
  assert (!parse ("{foo{:"));
  assert (!parse ("foo: bar:"));
  assert (!parse ("exe{foo:"));

  // Directory scope.
  //
  assert (parse ("test/:\n{\n}"));
  assert (parse ("test/:\n{\n}\n"));
  assert (parse ("test/:\n{\nfoo:bar\n}"));
  assert (parse ("test/:\n{\nfoo:bar\n}"));
  assert (parse ("test/:\n{\nmore/:\n{\n}\n}"));
  assert (parse ("test/:\n{\nmore/:\n{\nfoo:{bar baz}\n}\n}"));

  assert (!parse ("test/:\n{"));
  assert (!parse ("test/:\n{\n"));
  assert (!parse ("test/:\n{\n:"));
  assert (!parse ("test/:\n{\n} foo: bar\n"));
  assert (!parse ("test/ foo:\n{\n}"));
  assert (!parse ("test foo/:\n{\n}"));
  assert (!parse ("test/ foo/:\n{\n}"));
}

ostream cnull (nullptr);

static bool
parse (const char* s)
{
  istringstream is (s);

  is.exceptions (istream::failbit | istream::badbit);
  parser p (cnull);
  //parser p (cerr);

  try
  {
    p.parse (is, path ());
  }
  catch (const parser_error&)
  {
    return false;
  }

  return true;
}
