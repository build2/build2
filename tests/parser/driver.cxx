// file      : tests/build/parser/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <sstream>
#include <iostream>

#include <build/types>
#include <build/scope>
#include <build/target>
#include <build/context>
#include <build/variable>

#include <build/lexer>
#include <build/parser>

using namespace std;
using namespace build;

static bool
parse (const char*);

static names
parse_names (const char* s, lexer_mode m, bool chunk);

static names
chunk_names (const char* s)
{
  return parse_names (s, lexer_mode::pairs, true);
}

int
main ()
{
  ostream cnull (nullptr);
  diag_stream = &cnull;

  reset ();

  global_scope->assign ("foo") = "FOO";
  global_scope->assign ("bar") = "BAR";

  // names() in chunking mode.
  //
  assert (chunk_names ("{}") == names ({name ()}));
  assert (chunk_names ("foo") == names ({name ("foo")}));
  assert (chunk_names ("foo bar") == names ({name ("foo")}));
  assert (chunk_names ("{foo bar}") == names ({name ("foo"), name ("bar")}));
  assert (chunk_names ("dir{foo bar}") == names ({name ("dir", "foo"),
                                                  name ("dir", "bar")}));
  assert (chunk_names ("dir{foo bar} baz") == names ({name ("dir", "foo"),
                                                      name ("dir", "bar")}));
  assert (chunk_names ("dir {foo bar}") == names ({name ("dir", "foo"),
                                                   name ("dir", "bar")}));
  assert (chunk_names ("dir {foo bar} baz") == names ({name ("dir", "foo"),
                                                       name ("dir", "bar")}));
  assert (chunk_names ("{} foo") == names ({name ()}));

  // Expansion.
  //
  assert (chunk_names ("$foo $bar baz") == names ({name ("FOO")}));
  assert (chunk_names ("$foo$bar baz") == names ({name ("FOOBAR")}));

  assert (chunk_names ("foo(bar)") == names ({name ("foobar")}));
  assert (chunk_names ("foo (bar)") == names ({name ("foo")}));

  assert (chunk_names ("\"$foo\"(bar)") == names ({name ("FOObar")}));
  assert (chunk_names ("\"$foo\" (bar)") == names ({name ("FOO")}));

  // Quoting.
  //
  assert (chunk_names ("\"$foo $bar\" baz") == names ({name ("FOO BAR")}));

  // Pairs.
  //
  assert (chunk_names ("foo=bar") == names ({name ("foo"), name ("bar")}));
  assert (chunk_names ("foo = bar x") == names ({name ("foo"), name ("bar")}));

  // General.
  //
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

  assert (parse ("file{foo}:"));
  assert (parse ("file{foo bar}:"));
  assert (parse ("{file{foo bar}}:"));
  assert (parse ("file{{foo bar} fox}:"));
  assert (parse ("file{foo}: file{bar baz} biz.o file{fox}"));

  //assert (!parse (":"));
  assert (!parse ("foo"));
  assert (!parse ("{"));
  assert (!parse ("{foo:"));
  assert (!parse ("{foo{:"));
  assert (!parse ("foo: bar:"));
  assert (!parse ("file{foo:"));

  // Directory prefix.
  //
  assert (parse ("../{foo}: ../{bar}"));
  assert (parse ("../file{foo}: ../file{bar}"));
  assert (!parse ("../file{file{foo}}:"));

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

struct test_parser: parser
{
  names_type
  test_names (const char*, lexer_mode, bool chunk);
};

static bool
parse (const char* s)
{
  reset (); // Clear the state.

  // Create a minimal root scope.
  //
  auto i (scopes.insert (path::current (), nullptr, true, true));
  scope& root (*i->second);
  root.src_path_ = root.out_path_ = &i->first;

  istringstream is (s);

  is.exceptions (istream::failbit | istream::badbit);
  parser p;

  try
  {
    p.parse_buildfile (is, path (), root, root);
  }
  catch (const failed&)
  {
    return false;
  }

  return true;
}

// parser::names()
//
names test_parser::
test_names (const char* s, lexer_mode m, bool chunk)
{
  istringstream is (s);
  is.exceptions (istream::failbit | istream::badbit);
  lexer l (is, "");

  if (m != lexer_mode::normal)
    l.mode (m, '=');

  path_ = &l.name ();
  lexer_ = &l;
  target_ = nullptr;
  scope_ = root_ = global_scope;

  token t (token_type::eos, false, 0, 0);
  token_type tt;
  next (t, tt);
  return names (t, tt, chunk);
}

static names
parse_names (const char* s, lexer_mode m, bool chunk)
{
  test_parser p;
  return p.test_names (s, m, chunk);
}
