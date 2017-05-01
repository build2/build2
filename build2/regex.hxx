// file      : build2/regex.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_REGEX_HXX
#define BUILD2_REGEX_HXX

#include <regex>
#include <iosfwd>
#include <string> // basic_string

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  // Like std::regex_match() but extends the standard ECMA-262
  // substitution escape sequences with a subset of Perl sequences:
  //
  // \\, \u, \l, \U, \L, \E, \1, ..., \9
  //
  // Also return the resulting string as well as whether the search
  // succeeded.
  //
  // Notes and limitations:
  //
  // - The only valid regex_constants flags are match_default,
  //   format_first_only (format_no_copy can easily be supported).
  //
  // - If backslash doesn't start any of the listed sequences then it is
  //   silently dropped and the following character is copied as is.
  //
  // - The character case conversion is performed according to the global
  //   C++ locale (which is, unless changed, is the same as C locale and
  //   both default to the POSIX locale aka "C").
  //
  template <typename C>
  pair<std::basic_string<C>, bool>
  regex_replace_ex (const std::basic_string<C>&,
                    const std::basic_regex<C>&,
                    const std::basic_string<C>& fmt,
                    std::regex_constants::match_flag_type =
                      std::regex_constants::match_default);
}

namespace std
{
  // Print regex error description but only if it is meaningful (this is also
  // why we have to print leading colon).
  //
  ostream&
  operator<< (ostream&, const regex_error&);
}

#include <build2/regex.txx>

#endif // BUILD2_REGEX_HXX
