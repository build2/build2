// file      : tests/build/lexer/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <vector>
#include <sstream>
#include <iostream>

#include <build/token>
#include <build/lexer>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace build;

using tokens = vector<string>;

static tokens
lex (const char*);

ostream&
operator<< (ostream&, const tokens&);

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
  assert (lex ("foo\\\nbar") == tokens ({"foobar", ""}));
  assert (lex ("foo \\\nbar") == tokens ({"foo", "bar", ""}));
  assert (lex ("\\'foo") == tokens ({"'foo", ""}));

  assert (lex ("  \\") == tokens ({"<lexer error>"}));
  assert (lex ("  foo\\") == tokens ({"<lexer error>"}));


  // Quoting ''.
  //
  assert (lex ("''") == tokens ({"", ""}));
  assert (lex ("'foo'") == tokens ({"foo", ""}));
  assert (lex ("'foo bar'") == tokens ({"foo bar", ""}));
  assert (lex ("'foo 'bar") == tokens ({"foo bar", ""}));
  assert (lex ("foo' bar'") == tokens ({"foo bar", ""}));
  assert (lex ("'foo ''bar'") == tokens ({"foo bar", ""}));
  assert (lex ("foo' 'bar") == tokens ({"foo bar", ""}));
  assert (lex ("'foo\nbar'") == tokens ({"foo\nbar", ""}));
  assert (lex ("'#:${}()=+\n'") == tokens ({"#:${}()=+\n", ""}));
  assert (lex ("'\"'") == tokens ({"\"", ""}));
  assert (lex ("'\\'") == tokens ({"\\", ""}));

  assert (lex ("'foo bar") == tokens ({"<lexer error>"}));

  // Quoting "".
  //
  assert (lex ("\"\"") == tokens ({"", ""}));
  assert (lex ("\"foo\"") == tokens ({"foo", ""}));
  assert (lex ("\"foo bar\"") == tokens ({"foo bar", ""}));
  assert (lex ("\"foo \"bar") == tokens ({"foo bar", ""}));
  assert (lex ("foo\" bar\"") == tokens ({"foo bar", ""}));
  assert (lex ("\"foo \"\"bar\"") == tokens ({"foo bar", ""}));
  assert (lex ("foo\" \"bar") == tokens ({"foo bar", ""}));
  assert (lex ("\"foo\nbar\"") == tokens ({"foo\nbar", ""}));
  assert (lex ("\"#:{})=+\n\"") == tokens ({"#:{})=+\n", ""}));
  assert (lex ("\"'\"") == tokens ({"'", ""}));
  assert (lex ("\"\\\"") == tokens ({"\\", ""}));

  assert (lex ("\"$\"") == tokens ({"", "$", "", ""}));
  assert (lex ("\"foo$bar\"") == tokens ({"foo", "$", "bar", ""}));
  assert (lex ("foo\"$\"bar") == tokens ({"foo", "$", "bar", ""}));
  assert (lex ("f\"oo$ba\"r") == tokens ({"foo", "$", "bar", ""}));

  assert (lex ("\"foo bar") == tokens ({"<lexer error>"}));
  assert (lex ("\"foo $") == tokens ({"foo ", "$", "<lexer error>"}));
  assert (lex ("\"foo $bar") == tokens ({"foo ", "$", "<lexer error>"}));

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
      string v;

      switch (t.type)
      {
      case token_type::eos:            v = ""; break;
      case token_type::newline:        v = "\n"; break;
      case token_type::pair_separator: v = l.pair_separator (); break;
      case token_type::colon:          v = ":"; break;
      case token_type::lcbrace:        v = "{"; break;
      case token_type::rcbrace:        v = "}"; break;
      case token_type::equal:          v = "="; break;
      case token_type::plus_equal:     v = "+="; break;
      case token_type::dollar:         v = "$"; break;
      case token_type::lparen:         v = "("; break;
      case token_type::rparen:         v = ")"; break;
      case token_type::name:           v = t.value.c_str (); break;
      }

      // cerr << t.line () << ':' << t.column () << ':' << v << endl;

      r.push_back (move (v));

      if (t.type == token_type::eos)
        break;
    }
  }
  catch (const failed&)
  {
    r.push_back ("<lexer error>");
  }
  catch (const io_error&)
  {
    r.push_back ("<io error>");
  }

  return r;
}

ostream&
operator<< (ostream& os, const tokens& ts)
{
  for (const string& t: ts)
    os << '"' << t << '"' << ' ';

  return os;
}
