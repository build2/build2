// file      : build2/functions-regex.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <sstream>

#include <libbutl/regex.hxx>

#include <build2/function.hxx>
#include <build2/variable.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // Convert value of an arbitrary type to string.
  //
  static inline string
  to_string (value&& v)
  {
    // Optimize for the string value type.
    //
    if (v.type != &value_traits<string>::value_type)
      untypify (v);

    return convert<string> (move (v));
  }

  // Parse a regular expression. Throw invalid_argument if it is not valid.
  //
  static regex
  parse_regex (const string& s, regex::flag_type f)
  {
    try
    {
      return regex (s, f);
    }
    catch (const regex_error& e)
    {
      // Print regex_error description if meaningful (no space).
      //
      ostringstream os;
      os << "invalid regex '" << s << "'" << e;
      throw invalid_argument (os.str ());
    }
  }

  // Match value of an arbitrary type against the regular expression. See
  // match() overloads (below) for details.
  //
  static value
  match (value&& v, const string& re, optional<names>&& flags)
  {
    // Parse flags.
    //
    regex::flag_type rf (regex::ECMAScript);
    bool subs (false);

    if (flags)
    {
      for (auto& f: *flags)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          rf |= regex::icase;
        else if (s == "return_subs")
          subs = true;
        else
          throw invalid_argument ("invalid flag '" + s + "'");
      }
    }

    // Parse regex.
    //
    regex rge (parse_regex (re, rf));

    // Match.
    //
    string s (to_string (move (v)));

    if (!subs)
      return value (regex_match (s, rge)); // Return boolean value.

    names r;
    match_results<string::const_iterator> m;

    if (regex_match (s, m, rge))
    {
      assert (!m.empty ());

      for (size_t i (1); i != m.size (); ++i)
      {
        if (m[i].matched)
          r.emplace_back (m.str (i));
      }
    }

    return value (r);
  }

  // Determine if there is a match between the regular expression and some
  // part of a value of an arbitrary type. See search() overloads (below)
  // for details.
  //
  static value
  search (value&& v, const string& re, optional<names>&& flags)
  {
    // Parse flags.
    //
    regex::flag_type rf (regex::ECMAScript);
    bool match (false);
    bool subs (false);

    if (flags)
    {
      for (auto& f: *flags)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          rf |= regex::icase;
        else if (s == "return_match")
          match = true;
        else if (s == "return_subs")
          subs = true;
        else
          throw invalid_argument ("invalid flag '" + s + "'");
      }
    }

    // Parse regex.
    //
    regex rge (parse_regex (re, rf));

    // Search.
    //
    string s (to_string (move (v)));

    if (!match && !subs)
      return value (regex_search (s, rge)); // Return boolean value.

    names r;
    match_results<string::const_iterator> m;

    if (regex_search (s, m, rge))
    {
      assert (!m.empty ());

      if (match)
      {
        assert (m[0].matched);
        r.emplace_back (m.str (0));
      }

      if (subs)
      {
        for (size_t i (1); i != m.size (); ++i)
        {
          if (m[i].matched)
            r.emplace_back (m.str (i));
        }
      }
    }

    return value (r);
  }

  // Replace matched parts in a value of an arbitrary type, using the format
  // string. See replace() overloads (below) for details.
  //
  static names
  replace (value&& v,
           const string& re,
           const string& fmt,
           optional<names>&& flags)
  {
    // Parse flags.
    //
    regex::flag_type rf (regex::ECMAScript);
    regex_constants::match_flag_type mf (regex_constants::match_default);

    if (flags)
    {
      for (auto& f: *flags)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          rf |= regex::icase;
        else if (s == "format_first_only")
          mf |= regex_constants::format_first_only;
        else if (s == "format_no_copy")
          mf |= regex_constants::format_no_copy;
        else
          throw invalid_argument ("invalid flag '" + s + "'");
      }
    }

    // Parse regex.
    //
    regex rge (parse_regex (re, rf));

    // Replace.
    //
    names r;

    try
    {
      string s (to_string (move (v)));
      r.emplace_back (regex_replace_ex (s, rge, fmt, mf).first);
    }
    catch (const regex_error& e)
    {
      fail << "unable to replace" << e;
    }

    return r;
  }

  void
  regex_functions ()
  {
    function_family f ("regex");

    // match
    //
    // Match a value of an arbitrary type against the regular expression.
    // Convert the value to string prior to matching. Return the boolean value
    // unless return_subs flag is specified (see below), in which case return
    // names (empty if no match).
    //
    // The following flags are supported:
    //
    // icase       - match ignoring case
    //
    // return_subs - return names (rather than boolean), that contain
    //               sub-strings that match the marked sub-expressions
    //
    f[".match"] = [](value s, string re, optional<names> flags)
    {
      return match (move (s), re, move (flags));
    };

    f[".match"] = [](value s, names re, optional<names> flags)
    {
      return match (move (s), convert<string> (move (re)), move (flags));
    };

    // search
    //
    // Determine if there is a match between the regular expression and some
    // part of a value of an arbitrary type. Convert the value to string prior
    // to searching. Return the boolean value unless return_match or
    // return_subs flag is specified (see below) in which case return names
    // (empty if no match).
    //
    // The following flags are supported:
    //
    // icase        - match ignoring case
    //
    // return_match - return names (rather than boolean), that contain a
    //                sub-string that matches the whole regular expression
    //
    // return_subs -  return names (rather than boolean), that contain
    //                sub-strings that match the marked sub-expressions
    //
    // If both return_match and return_subs flags are specified then the
    // sub-string that matches the whole regular expression comes first.
    //
    f[".search"] = [](value s, string re, optional<names> flags)
    {
      return search (move (s), re, move (flags));
    };

    f[".search"] = [](value s, names re, optional<names> flags)
    {
      return search (move (s), convert<string> (move (re)), move (flags));
    };

    // replace
    //
    // Replace matched parts in a value of an arbitrary type, using the format
    // string. Convert the value to string prior to matching. The result value
    // is always untyped, regardless of the argument type.
    //
    // Substitution escape sequences are extended with a subset of Perl
    // sequences (see regex_replace_ex() for details).
    //
    // The following flags are supported:
    //
    // icase             - match ignoring case
    //
    // format_first_only - only replace the first match
    //
    // format_no_copy    - do not copy unmatched value parts to the result
    //
    // If both format_first_only and format_no_copy flags are specified then
    // all the result will contain is the replacement of the first match.
    //
    f[".replace"] = [](value s, string re, string fmt, optional<names> flags)
    {
      return replace (move (s), re, fmt, move (flags));
    };

    f[".replace"] = [](value s, string re, names fmt, optional<names> flags)
    {
      return replace (move (s),
                      re,
                      convert<string> (move (fmt)),
                      move (flags));
    };

    f[".replace"] = [](value s, names re, string fmt, optional<names> flags)
    {
      return replace (move (s),
                      convert<string> (move (re)),
                      fmt,
                      move (flags));
    };

    f[".replace"] = [](value s, names re, names fmt, optional<names> flags)
    {
      return replace (move (s),
                      convert<string> (move (re)),
                      convert<string> (move (fmt)),
                      move (flags));
    };
  }
}
