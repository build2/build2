// file      : tests/build/lexer/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <vector>
#include <cassert>
#include <sstream>
#include <iostream>

#include <build/token>
#include <build/lexer>

using namespace std;
using namespace build;

typedef vector<string> tokens;

static tokens
lex (const char*);

int
main ()
{
  ostream cnull (nullptr);
  diag_stream = &cnull;

  // Whitespaces.
  //
  assert (lex ("") == tokens ({""}));
  assert (lex ("\n") == tokens ({""}));
  assert (lex ("\n\n") == tokens ({""}));
  assert (lex (" \t  \n") == tokens ({""}));
  assert (lex ("#comment") == tokens ({""}));
  assert (lex ("  #comment") == tokens ({""}));
  assert (lex ("#comment\n") == tokens ({""}));
  assert (lex ("#comment\\\n") == tokens ({""}));
  assert (lex ("#comment 1\n#comment2") == tokens ({""}));

  // Punctuation.
  //
  assert (lex (": \n { }") == tokens ({":", "\n", "{", "}", ""}));

  // Names.
  //
  assert (lex ("foo") == tokens ({"foo", ""}));
  assert (lex ("foo.bar") == tokens ({"foo.bar", ""}));

  // Escaping.
  //
  assert (lex ("  \\\n") == tokens ({""}));
  assert (lex ("\\\nfoo") == tokens ({"foo", ""}));
  assert (lex (" \\ foo") == tokens ({" foo", ""}));
  assert (lex ("fo\\ o\\:") == tokens ({"fo o:", ""}));
  assert (lex ("foo\\\nbar") == tokens ({"foo\nbar", ""}));
  assert (lex ("foo \\\nbar") == tokens ({"foo", "bar", ""}));

  assert (lex ("  \\") == tokens ({"<lexer error>"}));
  assert (lex ("  foo\\") == tokens ({"<lexer error>"}));

  // Combinations.
  //
  assert (lex ("foo: bar") == tokens ({"foo", ":", "bar", ""}));
  assert (lex ("\n \nfoo: bar") == tokens ({"foo", ":", "bar", ""}));
  assert (lex ("foo: bar\n") == tokens ({"foo", ":", "bar", "\n", ""}));
  assert (lex ("foo: bar#comment") == tokens ({"foo", ":", "bar", ""}));
  assert (lex ("exe{foo}: obj{bar}") ==
          tokens ({"exe", "{", "foo", "}", ":", "obj", "{", "bar", "}", ""}));
  assert (lex ("foo: bar\nbaz: biz") ==
          tokens ({"foo", ":", "bar", "\n", "baz", ":", "biz", ""}));
  assert (lex ("foo: bar#comment\nbaz: biz") ==
          tokens ({"foo", ":", "bar", "\n", "baz", ":", "biz", ""}));
  assert (lex ("foo:#comment \\\nbar") ==
          tokens ({"foo", ":", "\n", "bar", ""}));
}

static tokens
lex (const char* s)
{
  tokens r;
  istringstream is (s);

  is.exceptions (istream::failbit | istream::badbit);
  lexer l (is, "");

  try
  {
    for (token t (l.next ());; t = l.next ())
    {
      const char* v (nullptr);

      switch (t.type ())
      {
      case token_type::eos: v= ""; break;
      case token_type::newline: v = "\n"; break;
      case token_type::colon:   v = ":"; break;
      case token_type::lcbrace: v = "{"; break;
      case token_type::rcbrace: v = "}"; break;
      case token_type::name: v = t.name ().c_str (); break;
      }

      // cerr << t.line () << ':' << t.column () << ':' << v << endl;

      r.push_back (v);

      if (t.type () == token_type::eos)
        break;
    }
  }
  catch (const failed&)
  {
    r.push_back ("<lexer error>");
  }
  catch (const std::ios_base::failure&)
  {
    r.push_back ("<io error>");
  }

  return r;
}
