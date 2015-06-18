// file      : tests/build/parser/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <sstream>
#include <iostream>

#include <build/types>
#include <build/scope>
#include <build/target>
#include <build/native>

#include <build/lexer>
#include <build/parser>

using namespace std;
using namespace build;

static bool
parse (const char*);

int
main ()
{
  ostream cnull (nullptr);
  diag_stream = &cnull;

  target_types.insert (file::static_type);
  target_types.insert (exe::static_type);
  target_types.insert (obj::static_type);

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
  assert (parse ("exe{foo}: obj{bar baz} biz.o file{fox}"));

  assert (!parse (":"));
  assert (!parse ("foo"));
  assert (!parse ("{"));
  assert (!parse ("{foo:"));
  assert (!parse ("{foo{:"));
  assert (!parse ("foo: bar:"));
  assert (!parse ("exe{foo:"));

  // Directory prefix.
  //
  assert (parse ("../{foo}: ../{bar}"));
  assert (parse ("../exe{foo}: ../obj{bar}"));
  assert (!parse ("../exe{exe{foo}}:"));

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

static bool
parse (const char* s)
{
  istringstream is (s);

  is.exceptions (istream::failbit | istream::badbit);
  parser p;

  try
  {
    p.parse (is, path (), scopes[path::current ()]);
  }
  catch (const failed&)
  {
    return false;
  }

  return true;
}
