// file      : libbuild2/functions-regex.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <sstream>

#include <libbutl/regex.hxx>

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

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
  // Note: also used in functions-process.cxx (thus not static).
  //
  regex
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
      for (name& f: *flags)
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

    match_results<string::const_iterator> m;

    if (regex_match (s, m, rge))
    {
      assert (!m.empty ());

      names r;
      for (size_t i (1); i != m.size (); ++i)
      {
        if (m[i].matched)
          r.emplace_back (m.str (i));
      }

      return value (move (r));
    }
    else
      return value ();
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

    match_results<string::const_iterator> m;

    if (regex_search (s, m, rge))
    {
      assert (!m.empty ());

      names r;

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

      return value (move (r));
    }
    else
      return value ();
  }

  static pair<regex::flag_type, regex_constants::match_flag_type>
  parse_replacement_flags (optional<names>&& flags, bool first_only = true)
  {
    regex::flag_type rf (regex::ECMAScript);
    regex_constants::match_flag_type mf (regex_constants::match_default);

    if (flags)
    {
      for (auto& f: *flags)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          rf |= regex::icase;
        else if (first_only && s == "format_first_only")
          mf |= regex_constants::format_first_only;
        else if (s == "format_no_copy")
          mf |= regex_constants::format_no_copy;
        else
          throw invalid_argument ("invalid flag '" + s + "'");
      }
    }

    return make_pair (rf, mf);
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
    auto fl (parse_replacement_flags (move (flags)));
    regex rge (parse_regex (re, fl.first));

    names r;

    try
    {
      r.emplace_back (regex_replace_search (to_string (move (v)),
                                            rge,
                                            fmt,
                                            fl.second).first);
    }
    catch (const regex_error& e)
    {
      // Print regex_error description if meaningful (no space).
      //
      fail << "unable to replace" << e;
    }

    return r;
  }

  // Replace matched parts in lines using the format string. See
  // replace_lines() overloads (below) for details.
  //
  static names
  replace_lines (value&& v,
                 const string& re,
                 const optional<string>& fmt,
                 optional<names>&& flags)
  {
    string s (to_string (move (v)));

    // Extract the return_lines flag, if present, and parse the remaining
    // flags using parse_replacement_flags().
    //
    bool rls (false);

    if (flags)
    {
      names& fs (*flags);
      const name rlf ("return_lines");

      for (names::const_iterator i (fs.begin ()); i != fs.end (); )
      {
        if (*i == rlf)
        {
          rls = true;
          i = fs.erase (i);
        }
        else
          ++i;
      }
    }

    auto fl (parse_replacement_flags (move (flags)));
    regex rge (parse_regex (re, fl.first));

    names r;
    string ls;

    try
    {
      istringstream is (s);
      is.exceptions (istringstream::badbit);

      const string& efmt (fmt ? *fmt : "");
      bool no_copy ((fl.second & regex_constants::format_no_copy) != 0);

      for (string l; !eof (getline (is, l)); )
      {
        auto rr (regex_replace_search (l, rge, efmt, fl.second));
        string& s (rr.first);

        // Skip the empty replacement for a matched line if the format is
        // absent and an unmatched line if the format_no_copy flag is
        // specified.
        //
        if (rr.second ? !fmt && s.empty () : no_copy)
          continue;

        if (!rls)
          r.emplace_back (to_name (move (s)));
        else
        {
          if (ls.empty ())
            ls = move (s);
          else
            ls += s;

          // Append the trailing newline for the added line if EOS is not
          // reached, which indicates that the original line is terminated
          // with newline.
          //
          if (!is.eof ())
            ls += '\n';
        }
      }
    }
    catch (const regex_error& e)
    {
      // Print regex_error description if meaningful (no space).
      //
      fail << "unable to replace lines" << e;
    }
    catch (const io_error& e)
    {
      fail << "unable to read lines: " << e;
    }

    if (rls)
      r.push_back (move (ls));

    return r;
  }

  // Split a value of an arbitrary type into a list of unmatched value parts
  // and replacements of the matched parts. See split() overloads (below) for
  // details.
  //
  static names
  split (value&& v,
         const string& re,
         const string& fmt,
         optional<names>&& flags)
  {
    auto fl (parse_replacement_flags (move (flags), false));
    regex rge (parse_regex (re, fl.first));

    names r;

    try
    {
      regex_replace_search (to_string (move (v)), rge, fmt,
                            [&r] (string::const_iterator b,
                                  string::const_iterator e)
                            {
                              if (b != e)
                                r.emplace_back (string (b, e));
                            },
                            fl.second);
    }
    catch (const regex_error& e)
    {
      // Print regex_error description if meaningful (no space).
      //
      fail << "unable to split" << e;
    }

    return r;
  }

  // Replace matched parts of list elements using the format string. See
  // apply() overloads (below) for details.
  //
  static names
  apply (names&& s,
         const string& re,
         const string& fmt,
         optional<names>&& flags)
  {
    auto fl (parse_replacement_flags (move (flags)));
    regex rge (parse_regex (re, fl.first));

    names r;

    try
    {
      for (auto& v: s)
      {
        string s (regex_replace_search (convert<string> (move (v)),
                                        rge,
                                        fmt,
                                        fl.second).first);

        if (!s.empty ())
          r.emplace_back (move (s));
      }
    }
    catch (const regex_error& e)
    {
      // Print regex_error description if meaningful (no space).
      //
      fail << "unable to apply" << e;
    }

    return r;
  }

  static regex::flag_type
  parse_find_flags (optional<names>&& flags)
  {
    regex::flag_type r (regex::ECMAScript);

    if (flags)
    {
      for (auto& f: *flags)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          r |= regex::icase;
        else
          throw invalid_argument ("invalid flag '" + s + "'");
      }
    }

    return r;
  }

  // Return true if any of the list elements match the regular expression.
  // See find_match() overloads (below) for details.
  //
  static bool
  find_match (names&& s, const string& re, optional<names>&& flags)
  {
    regex::flag_type fl (parse_find_flags (move (flags)));
    regex rge (parse_regex (re, fl));

    for (auto& v: s)
    {
      if (regex_match (convert<string> (move (v)), rge))
        return true;
    }

    return false;
  }

  // Return true if a part of any of the list elements matches the regular
  // expression. See find_search() overloads (below) for details.
  //
  static bool
  find_search (names&& s, const string& re, optional<names>&& flags)
  {
    regex::flag_type fl (parse_find_flags (move (flags)));
    regex rge (parse_regex (re, fl));

    for (auto& v: s)
    {
      if (regex_search (convert<string> (move (v)), rge))
        return true;
    }

    return false;
  }

  // Replace matched parts of list elements using the format string and
  // concatenate the transformed elements. See merge() overloads (below) for
  // details.
  //
  static names
  merge (names&& s,
         const string& re,
         const string& fmt,
         optional<string>&& delim,
         optional<names>&& flags)
  {
    auto fl (parse_replacement_flags (move (flags)));
    regex rge (parse_regex (re, fl.first));

    string rs;

    try
    {
      for (auto& v: s)
      {
        string s (regex_replace_search (convert<string> (move (v)),
                                        rge,
                                        fmt,
                                        fl.second).first);

        if (!s.empty ())
        {
          if (!rs.empty () && delim)
            rs.append (*delim);

          rs.append (s);
        }

      }
    }
    catch (const regex_error& e)
    {
      // Print regex_error description if meaningful (no space).
      //
      fail << "unable to merge" << e;
    }

    names r;
    r.emplace_back (move (rs));
    return r;
  }

  void
  regex_functions (function_map& m)
  {
    function_family f (m, "regex");

    // $regex.match(<val>, <pat> [, <flags>])
    //
    // Match a value of an arbitrary type against the regular expression.
    // Convert the value to string prior to matching. Return the boolean value
    // unless return_subs flag is specified (see below), in which case return
    // names (NULL if no match).
    //
    // The following flags are supported:
    //
    // icase       - match ignoring case
    //
    // return_subs - return names (rather than boolean), that contain
    //               sub-strings that match the marked sub-expressions and
    //               NULL if no match
    //
    f[".match"] += [](value s, string re, optional<names> flags)
    {
      return match (move (s), re, move (flags));
    };

    f[".match"] += [](value s, names re, optional<names> flags)
    {
      return match (move (s), convert<string> (move (re)), move (flags));
    };

    // $regex.find_match(<vals>, <pat> [, <flags>])
    //
    // Match list elements against the regular expression and return true if
    // the match is found. Convert the elements to string prior to matching.
    //
    // The following flags are supported:
    //
    // icase - match ignoring case
    //
    f[".find_match"] += [](names s, string re, optional<names> flags)
    {
      return find_match (move (s), re, move (flags));
    };

    f[".find_match"] += [](names s, names re, optional<names> flags)
    {
      return find_match (move (s), convert<string> (move (re)), move (flags));
    };

    // $regex.search(<val>, <pat> [, <flags>])
    //
    // Determine if there is a match between the regular expression and some
    // part of a value of an arbitrary type. Convert the value to string prior
    // to searching. Return the boolean value unless return_match or
    // return_subs flag is specified (see below) in which case return names
    // (NULL if no match).
    //
    // The following flags are supported:
    //
    // icase        - match ignoring case
    //
    // return_match - return names (rather than boolean), that contain a
    //                sub-string that matches the whole regular expression and
    //                NULL if no match
    //
    // return_subs  - return names (rather than boolean), that contain
    //                sub-strings that match the marked sub-expressions and
    //                NULL if no match
    //
    // If both return_match and return_subs flags are specified then the
    // sub-string that matches the whole regular expression comes first.
    //
    f[".search"] += [](value s, string re, optional<names> flags)
    {
      return search (move (s), re, move (flags));
    };

    f[".search"] += [](value s, names re, optional<names> flags)
    {
      return search (move (s), convert<string> (move (re)), move (flags));
    };

    // $regex.find_search(<vals>, <pat> [, <flags>])
    //
    // Determine if there is a match between the regular expression and some
    // part of any of the list elements. Convert the elements to string prior
    // to matching.
    //
    // The following flags are supported:
    //
    // icase - match ignoring case
    //
    f[".find_search"] += [](names s, string re, optional<names> flags)
    {
      return find_search (move (s), re, move (flags));
    };

    f[".find_search"] += [](names s, names re, optional<names> flags)
    {
      return find_search (move (s),
                          convert<string> (move (re)),
                          move (flags));
    };

    // $regex.replace(<val>, <pat>, <fmt> [, <flags>])
    //
    // Replace matched parts in a value of an arbitrary type, using the format
    // string. Convert the value to string prior to matching. The result value
    // is always untyped, regardless of the argument type.
    //
    // Substitution escape sequences are extended with a subset of Perl
    // sequences (see libbutl/regex.hxx for details).
    //
    // The following flags are supported:
    //
    // icase             - match ignoring case
    //
    // format_first_only - only replace the first match
    //
    // format_no_copy    - do not copy unmatched value parts into the result
    //
    // If both format_first_only and format_no_copy flags are specified then
    // the result will only contain the replacement of the first match.
    //
    f[".replace"] += [](value s, string re, string fmt, optional<names> flags)
    {
      return replace (move (s), re, fmt, move (flags));
    };

    f[".replace"] += [](value s, names re, names fmt, optional<names> flags)
    {
      return replace (move (s),
                      convert<string> (move (re)),
                      convert<string> (move (fmt)),
                      move (flags));
    };

    // $regex.replace_lines(<val>, <pat>, <fmt> [, <flags>])
    //
    // Convert the value to string, parse it into lines and for each line
    // apply the $regex.replace() function with the specified pattern, format,
    // and flags. If the format argument is NULL, omit the "all-NULL"
    // replacements for the matched lines from the result. Return unmatched
    // lines and line replacements as a name list unless return_lines flag is
    // specified (see below), in which case return a single multi-line simple
    // name value.
    //
    // The following flags are supported in addition to the $regex.replace()
    // function flags:
    //
    // return_lines - return the simple name (rather than a name list)
    //                containing the unmatched lines and line replacements
    //                separated with newlines.
    //
    // Note that if format_no_copy is specified, unmatched lines are not
    // copied either.
    //
    f[".replace_lines"] += [](value s,
                             string re,
                             string fmt,
                             optional<names> flags)
    {
      return replace_lines (move (s), re, move (fmt), move (flags));
    };

    f[".replace_lines"] += [](value s,
                             names re,
                             names* fmt,
                             optional<names> flags)
    {
      return replace_lines (
        move (s),
        convert<string> (move (re)),
        (fmt != nullptr
         ? optional<string> (convert<string> (move (*fmt)))
         : nullopt),
        move (flags));
    };

    // $regex.split(<val>, <pat>, <fmt> [, <flags>])
    //
    // Split a value of an arbitrary type into a list of unmatched value parts
    // and replacements of the matched parts, omitting empty ones. Convert the
    // value to string prior to matching.
    //
    // Substitution escape sequences are extended with a subset of Perl
    // sequences (see libbutl/regex.hxx for details).
    //
    // The following flags are supported:
    //
    // icase             - match ignoring case
    //
    // format_no_copy    - do not copy unmatched value parts into the result
    //
    f[".split"] += [](value s, string re, string fmt, optional<names> flags)
    {
      return split (move (s), re, fmt, move (flags));
    };

    f[".split"] += [](value s, names re, names fmt, optional<names> flags)
    {
      return split (move (s),
                    convert<string> (move (re)),
                    convert<string> (move (fmt)),
                    move (flags));
    };

    // $regex.merge(<vals>, <pat>, <fmt> [, <delim> [, <flags>]])
    //
    // Replace matched parts in a list of elements using the regex format
    // string. Convert the elements to string prior to matching. The result
    // value is untyped and contains concatenation of transformed non-empty
    // elements optionally separated with a delimiter.
    //
    // Substitution escape sequences are extended with a subset of Perl
    // sequences (see libbutl/regex.hxx for details).
    //
    // The following flags are supported:
    //
    // icase             - match ignoring case
    //
    // format_first_only - only replace the first match
    //
    // format_no_copy    - do not copy unmatched value parts into the result
    //
    // If both format_first_only and format_no_copy flags are specified then
    // the result will be a concatenation of only the first match
    // replacements.
    //
    f[".merge"] += [](names s,
                     string re,
                     string fmt,
                     optional<string> delim,
                     optional<names> flags)
    {
      return merge (move (s), re, fmt, move (delim), move (flags));
    };

    f[".merge"] += [](names s,
                     names re,
                     names fmt,
                     optional<names> delim,
                     optional<names> flags)
    {
      return merge (move (s),
                    convert<string> (move (re)),
                    convert<string> (move (fmt)),
                    delim
                    ? convert<string> (move (*delim))
                    : optional<string> (),
                    move (flags));
    };

    // $regex.apply(<vals>, <pat>, <fmt> [, <flags>])
    //
    // Replace matched parts of each element in a list using the regex format
    // string. Convert the elements to string prior to matching. Return a list
    // of transformed elements, omitting the empty ones.
    //
    // Substitution escape sequences are extended with a subset of Perl
    // sequences (see libbutl/regex.hxx for details).
    //
    // The following flags are supported:
    //
    // icase             - match ignoring case
    //
    // format_first_only - only replace the first match
    //
    // format_no_copy    - do not copy unmatched value parts into the result
    //
    // If both format_first_only and format_no_copy flags are specified then
    // the result elements will only contain the replacement of the first
    // match.
    //
    f[".apply"] += [](names s, string re, string fmt, optional<names> flags)
    {
      return apply (move (s), re, fmt, move (flags));
    };

    f[".apply"] += [](names s, names re, names fmt, optional<names> flags)
    {
      return apply (move (s),
                    convert<string> (move (re)),
                    convert<string> (move (fmt)),
                    move (flags));
    };
  }
}
