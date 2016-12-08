// file      : build2/test/script/regex.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/regex>

#include <algorithm> // copy(), copy_backward()

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      namespace regex
      {
        const line_char line_char::nul (0);
        const line_char line_char::eof (-1);

        // line_char
        //
        line_char::
        line_char (int c)
            : type (line_type::special), special (c)
        {
          // @@ How can we allow anything for basic_regex but only subset
          //    for our own code?
          //
          const char sp[] = "()|.*+?{\\}0123456789,=!";
          const char ex[] = "pn\n\r";

          assert (c == 0  || // Null character.

                  // EOF. Note that is also passed by msvcrt as _Meta_eos
                  // enum value.
                  //
                  c == -1 ||

                  // libstdc++ line/paragraph separators.
                  //
                  c == u'\u2028' || c == u'\u2029' ||

                  (c > 0 && c <= 255 && (
                    // Supported regex special characters.
                    //
                    string::traits_type::find (sp, 23, c) != nullptr ||

                    // libstdc++ look-ahead tokens, newline chars.
                    //
                    string::traits_type::find (ex, 4, c) != nullptr)));
        }

        line_char::
        line_char (const char_string& s, line_pool& p)
            : line_char (&(*p.strings.emplace (s).first))
        {
        }

        line_char::
        line_char (char_string&& s, line_pool& p)
            : line_char (&(*p.strings.emplace (move (s)).first))
        {
        }

        line_char::
        line_char (char_regex r, line_pool& p)
            // Note: in C++17 can write as p.regexes.emplace_front(move (r))
            //
            : line_char (&(*p.regexes.emplace (p.regexes.begin (), move (r))))
        {
        }

        bool
        operator== (const line_char& l, const line_char& r)
        {
          if (l.type == r.type)
          {
            bool res (true);

            switch (l.type)
            {
            case line_type::special: res = l.special == r.special; break;
            case line_type::regex:   assert (false); break;

              // Note that we use pointers (rather than vales) comparison
              // assuming that the strings must belong to the same pool.
              //
            case line_type::literal: res = l.literal == r.literal; break;
            }

            return res;
          }

          // Match literal with regex.
          //
          if (l.type == line_type::literal && r.type == line_type::regex)
            return regex_match (*l.literal, *r.regex);
          else if (r.type == line_type::literal && l.type == line_type::regex)
            return regex_match (*r.literal, *l.regex);

          return false;
        }

        bool
        operator< (const line_char& l, const line_char& r)
        {
          if (l == r)
            return false;

          if (l.type != r.type)
            return l.type < r.type;

          bool res (false);

          switch (l.type)
          {
          case line_type::special: res = l.special < r.special; break;
          case line_type::literal: res = *l.literal < *r.literal; break;
          case line_type::regex:   assert (false); break;
          }

          return res;
        }

        // line_char_locale
        //
        line_char_locale::
        line_char_locale ()
            : locale (locale (),
                      new std::ctype<line_char> ()) // Hidden by ctype bitmask.
        {
        }
      }
    }
  }
}

namespace std
{
  using namespace build2::test::script::regex;

  // char_traits<line_char>
  //
  line_char* char_traits<line_char>::
  assign (char_type* s, size_t n, char_type c)
  {
    for (size_t i (0); i != n; ++i)
      s[i] = c;
    return s;
  }

  line_char* char_traits<line_char>::
  move (char_type* d, const char_type* s, size_t n)
  {
    if (n > 0 && d != s)
    {
      // If d < s then it can't be in [s, s + n) range and so using copy() is
      // safe. Otherwise d + n is out of (first, last] range and so using
      // copy_backward() is safe.
      //
      if (d < s)
        std::copy (s, s + n, d); // Hidden by char_traits<line_char>::copy().
      else
        copy_backward (s, s + n, d + n);
    }

    return d;
  }

  line_char* char_traits<line_char>::
  copy (char_type* d, const char_type* s, size_t n)
  {
    std::copy (s, s + n, d); // Hidden by char_traits<line_char>::copy().
    return d;
  }

  int char_traits<line_char>::
  compare (const char_type* s1, const char_type* s2, size_t n)
  {
    for (size_t i (0); i != n; ++i)
    {
      if (s1[i] < s2[i])
        return -1;
      else if (s2[i] < s1[i])
        return 1;
    }

    return 0;
  }

  size_t char_traits<line_char>::
  length (const char_type* s)
  {
    size_t i (0);
    while (s[i] != char_type::nul)
      ++i;

    return i;
  }

  const line_char* char_traits<line_char>::
  find (const char_type* s, size_t n, const char_type& c)
  {
    for (size_t i (0); i != n; ++i)
    {
      if (s[i] == c)
        return s + i;
    }

    return nullptr;
  }

  // ctype<line_char>
  //
  locale::id ctype<line_char>::id;

  const line_char* ctype<line_char>::
  is (const char_type* b, const char_type* e, mask* m) const
  {
    while (b != e)
    {
      const char_type& c (*b++);

      *m++ = c.type == line_type::special && build2::digit (c.special)
        ? digit
        : 0;
    }

    return e;
  }

  const line_char* ctype<line_char>::
  scan_is (mask m, const char_type* b, const char_type* e) const
  {
    for (; b != e; ++b)
    {
      if (is (m, *b))
        return b;
    }

    return e;
  }

  const line_char* ctype<line_char>::
  scan_not (mask m, const char_type* b, const char_type* e) const
  {
    for (; b != e; ++b)
    {
      if (!is (m, *b))
        return b;
    }

    return e;
  }

  const char* ctype<line_char>::
  widen (const char* b, const char* e, char_type* c) const
  {
    while (b != e)
      *c++ = widen (*b++);

    return e;
  }

  const line_char* ctype<line_char>::
  narrow (const char_type* b, const char_type* e, char def, char* c) const
  {
    while (b != e)
      *c++ = narrow (*b++, def);

    return e;
  }

  // regex_traits<line_char>
  //
  int regex_traits<line_char>::
  value (char_type c, int radix) const
  {
    assert (radix == 8 || radix == 10 || radix == 16);

    if (c.type != line_type::special)
      return -1;

    const char digits[] = "0123456789ABCDEF";
    const char* d (string::traits_type::find (digits, radix, c.special));
    return d != nullptr ? d - digits : -1;
  }
}
