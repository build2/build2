// file      : libbuild2/script/regex.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <locale>

#include <libbuild2/script/regex.hxx>

using namespace std;

namespace build2
{
  namespace script
  {
    namespace regex
    {
      static_assert (alignof (char_string) % 4 == 0,
                     "unexpected char_string alignment");

      static_assert (alignof (char_regex) % 4 == 0,
                     "unexpected char_regex alignment");

      static_assert (sizeof (uintptr_t) > sizeof (int16_t),
                     "unexpected uintptr_t size");

      const line_char line_char::nul (0);
      const line_char line_char::eof (-1);

      // line_char
      //
      // We package the special character into uintptr_t with the following
      // steps:
      //
      // - narrow down int value to int16_t (preserves all the valid values)
      //
      // - convert to uint16_t (bitwise representation stays the same, but no
      //   need to bother with signed value widening, leftmost bits loss on
      //   left shift, etc)
      //
      // - convert to uintptr_t (storage type)
      //
      // - shift left by two bits (the operation is fully reversible as
      //   uintptr_t is wider then uint16_t)
      //
      line_char::
      line_char (int c)
          : data_ (
            (static_cast <uintptr_t> (
              static_cast<uint16_t> (
                static_cast<int16_t> (c))) << 2) |
            static_cast <uintptr_t> (line_type::special))
      {
        // @@ How can we allow anything for basic_regex but only subset
        //    for our own code?
        //
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
                  syntax (c) ||

                  // libstdc++ look-ahead tokens, newline chars.
                  //
                  string::traits_type::find (ex, 4, c) != nullptr)));
      }

      template <typename S>
      static inline const char_string*
      find_or_insert (line_pool& p, S&& s)
      {
        auto i (find (p.strings.begin (), p.strings.end (), s));
        if (i == p.strings.end ())
        {
          p.strings.push_front (forward<S> (s));
          i = p.strings.begin ();
        }

        return &*i;
      }

      line_char::
      line_char (const char_string& s, line_pool& p)
          : line_char (find_or_insert (p, s))
      {
      }

      line_char::
      line_char (char_string&& s, line_pool& p)
          : line_char (find_or_insert (p, move (s)))
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
      line_char::syntax (char c)
      {
        return string::traits_type::find (
          "()|.*+?{}\\0123456789,=!", 23, c) != nullptr;
      }

      bool
      operator== (const line_char& l, const line_char& r)
      {
        line_type lt (l.type ());
        line_type rt (r.type ());

        if (lt == rt)
        {
          bool res (true);

          switch (lt)
          {
          case line_type::special: res = l.special () == r.special (); break;
          case line_type::regex:   assert (false); break;

            // Note that we use pointers (rather than vales) comparison
            // assuming that the strings must belong to the same pool.
            //
          case line_type::literal: res = l.literal () == r.literal (); break;
          }

          return res;
        }

        // Match literal with regex.
        //
        if (lt == line_type::literal && rt == line_type::regex)
          return regex_match (*l.literal (), *r.regex ());
        else if (rt == line_type::literal && lt == line_type::regex)
          return regex_match (*r.literal (), *l.regex ());

        return false;
      }

      bool
      operator< (const line_char& l, const line_char& r)
      {
        if (l == r)
          return false;

        line_type lt (l.type ());
        line_type rt (r.type ());

        if (lt != rt)
          return lt < rt;

        bool res (false);

        switch (lt)
        {
        case line_type::special: res =  l.special () <  r.special (); break;
        case line_type::literal: res = *l.literal () < *r.literal (); break;
        case line_type::regex:   assert (false); break;
        }

        return res;
      }

      // line_char_locale
      //

      // An exemplar locale with the std::ctype<line_char> facet. It is used
      // for the subsequent line char locale objects creation (see below)
      // which normally ends up with a shallow copy of a reference-counted
      // object.
      //
      // Note that creating the line char locales from the exemplar is not
      // merely an optimization: there is a data race in the libstdc++ (at
      // least as of GCC 9.1) implementation of the locale(const locale&,
      // Facet*) constructor (bug #91057).
      //
      // Also note that we install the facet in init() rather than during
      // the object creation to avoid a race with the std::locale-related
      // global variables initialization.
      //
      static locale line_char_locale_exemplar;

      void
      init ()
      {
        line_char_locale_exemplar =
          locale (locale (),
                  new std::ctype<line_char> ()); // Hidden by ctype bitmask.
      }

      line_char_locale::
      line_char_locale ()
          : locale (line_char_locale_exemplar)
      {
        // Make sure init() has been called.
        //
        // Note: has_facet() is hidden by a private function in libc++.
        //
        assert (std::has_facet<std::ctype<line_char>> (*this));
      }

      // char_regex
      //
      // Transform regex according to the extended flags {idot}. If regex is
      // malformed then keep transforming, so the resulting string is
      // malformed the same way. We expect the error to be reported by the
      // char_regex ctor.
      //
      static string
      transform (const string& s, char_flags f)
      {
        assert ((f & char_flags::idot) != char_flags::none);

        string r;
        bool escape (false);
        bool cclass (false);

        for (char c: s)
        {
          // Inverse escaping for a dot which is out of the char class
          // brackets.
          //
          bool inverse (c == '.' && !cclass);

          // Handle the escape case. Note that we delay adding the backslash
          // since we may have to inverse things.
          //
          if (escape)
          {
            if (!inverse)
              r += '\\';

            r += c;
            escape = false;

            continue;
          }
          else if (c == '\\')
          {
            escape = true;
            continue;
          }

          // Keep track of being inside the char class brackets, escape if
          // inversion. Note that we never inverse square brackets.
          //
          if (c == '[' && !cclass)
            cclass = true;
          else if (c == ']' && cclass)
            cclass = false;
          else if (inverse)
            r += '\\';

          r += c;
        }

        if (escape) // Regex is malformed but that's not our problem.
          r += '\\';

        return r;
      }

      static char_regex::flag_type
      to_std_flags (char_flags f)
      {
        // Note that ECMAScript flag is implied in the absense of a grammar
        // flag.
        //
        return (f & char_flags::icase) != char_flags::none
          ? char_regex::icase
          : char_regex::flag_type ();
      }

      char_regex::
      char_regex (const char_string& s, char_flags f)
          : base_type ((f & char_flags::idot) != char_flags::none
                       ? transform (s, f)
                       : s,
                       to_std_flags (f))
      {
      }
    }
  }
}

namespace std
{
  using namespace build2::script::regex;

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
      // safe. Otherwise d + n is out of (s, s + n] range and so using
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

      *m++ = c.type () == line_type::special && c.special () >= 0 &&
        build2::digit (static_cast<char> (c.special ()))
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

    if (c.type () != line_type::special)
      return -1;

    const char digits[] = "0123456789ABCDEF";
    const char* d (string::traits_type::find (digits, radix, c.special ()));
    return d != nullptr ? static_cast<int> (d - digits) : -1;
  }
}
