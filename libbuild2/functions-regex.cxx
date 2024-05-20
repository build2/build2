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
      untypify (v, true /* reduce */);

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
          throw invalid_argument ("invalid flag '" + s + '\'');
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
        r.emplace_back (m[i].matched ? m.str (i) : string ());

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
          throw invalid_argument ("invalid flag '" + s + '\'');
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
          r.emplace_back (m[i].matched ? m.str (i) : string ());
      }

      return value (move (r));
    }
    else
      return value ();
  }

  static pair<regex::flag_type, regex_constants::match_flag_type>
  parse_replacement_flags (optional<names>&& flags,
                           bool first_only = true,
                           bool* copy_empty = nullptr)
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
        else if (copy_empty != nullptr && s == "format_copy_empty")
          *copy_empty = true;
        else
          throw invalid_argument ("invalid flag '" + s + '\'');
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
    bool copy_empty (false);
    auto fl (parse_replacement_flags (move (flags),
                                      false /* first_only */,
                                      &copy_empty));
    regex rge (parse_regex (re, fl.first));

    names r;

    try
    {
      regex_replace_search (to_string (move (v)), rge, fmt,
                            [copy_empty, &r] (string::const_iterator b,
                                              string::const_iterator e)
                            {
                              if (copy_empty || b != e)
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
  apply (names&& ns,
         const string& re,
         const string& fmt,
         optional<names>&& flags)
  {
    bool copy_empty (false);
    auto fl (parse_replacement_flags (move (flags),
                                      true /* first_only */,
                                      &copy_empty));
    regex rge (parse_regex (re, fl.first));

    names r;

    try
    {
      for (auto& n: ns)
      {
        string s (regex_replace_search (convert<string> (move (n)),
                                        rge,
                                        fmt,
                                        fl.second).first);

        if (copy_empty || !s.empty ())
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
          throw invalid_argument ("invalid flag '" + s + '\'');
      }
    }

    return r;
  }

  // Return true if any of the list elements match the regular expression.
  // See find_match() overloads (below) for details.
  //
  static bool
  find_match (names&& ns, const string& re, optional<names>&& flags)
  {
    regex::flag_type fl (parse_find_flags (move (flags)));
    regex rge (parse_regex (re, fl));

    for (auto& n: ns)
    {
      if (regex_match (convert<string> (move (n)), rge))
        return true;
    }

    return false;
  }

  // Return a list of elements that match (matching is true) or don't match
  // (matching is false) the regular expression. See filter_match() and
  // filter_out_match() overloads (below) for details.
  //
  static names
  filter_match (names&& ns,
                const string& re,
                optional<names>&& flags,
                bool matching)
  {
    regex::flag_type fl (parse_find_flags (move (flags)));
    regex rge (parse_regex (re, fl));

    names r;

    for (name& n: ns)
    {
      // Note that we need to preserve the element while converting it to
      // string since we may add it to the resulting list. But let's optimize
      // this for the simple value case by round-tripping it through the
      // string.
      //
      bool s (n.simple ());
      string v (convert<string> (s ? move (n) : name (n)));

      if (regex_match (v, rge) == matching)
        r.emplace_back (s ? name (move (v)) : move (n));
    }

    return r;
  }

  // Return true if a part of any of the list elements matches the regular
  // expression. See find_search() overloads (below) for details.
  //
  static bool
  find_search (names&& ns, const string& re, optional<names>&& flags)
  {
    regex::flag_type fl (parse_find_flags (move (flags)));
    regex rge (parse_regex (re, fl));

    for (auto& n: ns)
    {
      if (regex_search (convert<string> (move (n)), rge))
        return true;
    }

    return false;
  }

  // Return those elements of a list which have a match (matching is true) or
  // have no match (matching is false) between the regular expression and
  // some/any part of the element. See filter_search() and filter_out_search()
  // overloads (below) for details.
  //
  static names
  filter_search (names&& ns,
                 const string& re,
                 optional<names>&& flags,
                 bool matching)
  {
    regex::flag_type fl (parse_find_flags (move (flags)));
    regex rge (parse_regex (re, fl));

    names r;

    for (auto& n: ns)
    {
      // Note that we need to preserve the element while converting it to
      // string since we may add it to the resulting list. But let's optimize
      // this for the simple value case by round-tripping it through the
      // string.
      //
      bool s (n.simple ());
      string v (convert<string> (s ? move (n) : name (n)));

      if (regex_search (v, rge) == matching)
        r.emplace_back (s ? name (move (v)) : move (n));
    }

    return r;
  }

  // Replace matched parts of list elements using the format string and
  // concatenate the transformed elements. See merge() overloads (below) for
  // details.
  //
  static names
  merge (names&& ns,
         const string& re,
         const string& fmt,
         optional<string>&& delim,
         optional<names>&& flags)
  {
    bool copy_empty (false);
    auto fl (parse_replacement_flags (move (flags),
                                      true /* first_only */,
                                      &copy_empty));
    regex rge (parse_regex (re, fl.first));

    string rs;

    try
    {
      bool first (true);
      for (auto& n: ns)
      {
        string s (regex_replace_search (convert<string> (move (n)),
                                        rge,
                                        fmt,
                                        fl.second).first);

        if (copy_empty || !s.empty ())
        {
          if (delim)
          {
            if (first)
              first = false;
            else
              rs.append (*delim);
          }

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
    // unless `return_subs` flag is specified (see below), in which case
    // return names (or `null` if no match).
    //
    // The following flags are supported:
    //
    //     icase       - match ignoring case
    //
    //     return_subs - return names (rather than boolean), that contain
    //                   sub-strings that match the marked sub-expressions
    //                   and null if no match
    //
    f[".match"] += [](value v, string re, optional<names> flags)
    {
      return match (move (v), re, move (flags));
    };

    f[".match"] += [](value v, names re, optional<names> flags)
    {
      return match (move (v), convert<string> (move (re)), move (flags));
    };

    // $regex.find_match(<vals>, <pat> [, <flags>])
    //
    // Match list elements against the regular expression and return true if
    // the match is found. Convert the elements to strings prior to matching.
    //
    // The following flags are supported:
    //
    //     icase - match ignoring case
    //
    f[".find_match"] += [](names ns, string re, optional<names> flags)
    {
      return find_match (move (ns), re, move (flags));
    };

    f[".find_match"] += [](names ns, names re, optional<names> flags)
    {
      return find_match (move (ns), convert<string> (move (re)), move (flags));
    };

    // $regex.filter_match(<vals>, <pat> [, <flags>])
    // $regex.filter_out_match(<vals>, <pat> [, <flags>])
    //
    // Return elements of a list that match (`filter`) or do not match
    // (`filter_out`) the regular expression. Convert the elements to strings
    // prior to matching.
    //
    // The following flags are supported:
    //
    //     icase - match ignoring case
    //
    f[".filter_match"] += [](names ns, string re, optional<names> flags)
    {
      return filter_match (move (ns), re, move (flags), true /* matching */);
    };

    f[".filter_match"] += [](names ns, names re, optional<names> flags)
    {
      return filter_match (move (ns),
                           convert<string> (move (re)),
                           move (flags),
                           true /* matching */);
    };

    f[".filter_out_match"] += [](names s, string re, optional<names> flags)
    {
      return filter_match (move (s), re, move (flags), false /* matching */);
    };

    f[".filter_out_match"] += [](names ns, names re, optional<names> flags)
    {
      return filter_match (move (ns),
                           convert<string> (move (re)),
                           move (flags),
                           false /* matching */);
    };

    // $regex.search(<val>, <pat> [, <flags>])
    //
    // Determine if there is a match between the regular expression and some
    // part of a value of an arbitrary type. Convert the value to string prior
    // to searching. Return the boolean value unless `return_match` or
    // `return_subs` flag is specified (see below) in which case return names
    // (`null` if no match).
    //
    // The following flags are supported:
    //
    //     icase        - match ignoring case
    //
    //     return_match - return names (rather than boolean), that contain a
    //                    sub-string that matches the whole regular expression
    //                    and null if no match
    //
    //     return_subs  - return names (rather than boolean), that contain
    //                    sub-strings that match the marked sub-expressions
    //                    and null if no match
    //
    // If both `return_match` and `return_subs` flags are specified then the
    // sub-string that matches the whole regular expression comes first.
    //
    // See also `$string.contains()`, `$string.starts_with()`,
    // `$string.ends_with()`.
    //
    f[".search"] += [](value v, string re, optional<names> flags)
    {
      return search (move (v), re, move (flags));
    };

    f[".search"] += [](value v, names re, optional<names> flags)
    {
      return search (move (v), convert<string> (move (re)), move (flags));
    };

    // $regex.find_search(<vals>, <pat> [, <flags>])
    //
    // Determine if there is a match between the regular expression and some
    // part of any of the list elements. Convert the elements to strings prior
    // to matching.
    //
    // The following flags are supported:
    //
    //     icase - match ignoring case
    //
    f[".find_search"] += [](names ns, string re, optional<names> flags)
    {
      return find_search (move (ns), re, move (flags));
    };

    f[".find_search"] += [](names ns, names re, optional<names> flags)
    {
      return find_search (move (ns),
                          convert<string> (move (re)),
                          move (flags));
    };

    // $regex.filter_search(<vals>, <pat> [, <flags>])
    // $regex.filter_out_search(<vals>, <pat> [, <flags>])
    //
    // Return elements of a list for which there is a match (`filter`) or no
    // match (`filter_out`) between the regular expression and some part of
    // the element. Convert the elements to strings prior to matching.
    //
    // The following flags are supported:
    //
    //     icase - match ignoring case
    //
    f[".filter_search"] += [](names ns, string re, optional<names> flags)
    {
      return filter_search (move (ns), re, move (flags), true /* matching */);
    };

    f[".filter_search"] += [](names ns, names re, optional<names> flags)
    {
      return filter_search (move (ns),
                            convert<string> (move (re)),
                            move (flags),
                            true /* matching */);
    };

    f[".filter_out_search"] += [](names ns, string re, optional<names> flags)
    {
      return filter_search (move (ns), re, move (flags), false /* matching */);
    };

    f[".filter_out_search"] += [](names ns, names re, optional<names> flags)
    {
      return filter_search (move (ns),
                            convert<string> (move (re)),
                            move (flags),
                            false /* matching */);
    };

    // $regex.replace(<val>, <pat>, <fmt> [, <flags>])
    //
    // Replace matched parts in a value of an arbitrary type, using the format
    // string. Convert the value to string prior to matching. The result value
    // is always untyped, regardless of the argument type.
    //
    // The following flags are supported:
    //
    //     icase             - match ignoring case
    //
    //     format_first_only - only replace the first match
    //
    //     format_no_copy    - do not copy unmatched value parts into the
    //                         result
    //
    // If both `format_first_only` and `format_no_copy` flags are specified
    // then the result will only contain the replacement of the first match.
    //
    // See also `$string.replace()`.
    //
    f[".replace"] += [](value v, string re, string fmt, optional<names> flags)
    {
      return replace (move (v), re, fmt, move (flags));
    };

    f[".replace"] += [](value v, names re, names fmt, optional<names> flags)
    {
      return replace (move (v),
                      convert<string> (move (re)),
                      convert<string> (move (fmt)),
                      move (flags));
    };

    // $regex.replace_lines(<val>, <pat>, <fmt> [, <flags>])
    //
    // Convert the value to string, parse it into lines and for each line
    // apply the `$regex.replace()` function with the specified pattern,
    // format, and flags. If the format argument is `null`, omit the
    // "all-`null`" replacements for the matched lines from the result. Return
    // unmatched lines and line replacements as a `name` list unless
    // `return_lines` flag is specified (see below), in which case return a
    // single multi-line simple `name` value.
    //
    // The following flags are supported in addition to the `$regex.replace()`
    // function's flags:
    //
    //     return_lines - return the simple name (rather than a name list)
    //                    containing the unmatched lines and line replacements
    //                    separated with newlines.
    //
    // Note that if `format_no_copy` is specified, unmatched lines are not
    // copied either.
    //
    f[".replace_lines"] += [](value v,
                              string re,
                              string fmt,
                              optional<names> flags)
    {
      return replace_lines (move (v), re, move (fmt), move (flags));
    };

    f[".replace_lines"] += [](value v,
                              names re,
                              names* fmt,
                              optional<names> flags)
    {
      return replace_lines (
        move (v),
        convert<string> (move (re)),
        (fmt != nullptr
         ? optional<string> (convert<string> (move (*fmt)))
         : nullopt),
        move (flags));
    };

    // $regex.split(<val>, <pat>, <fmt> [, <flags>])
    //
    // Split a value of an arbitrary type into a list of unmatched value parts
    // and replacements of the matched parts, omitting empty ones (unless the
    // `format_copy_empty` flag is specified). Convert the value to string
    // prior to matching.
    //
    // The following flags are supported:
    //
    //     icase             - match ignoring case
    //
    //     format_no_copy    - do not copy unmatched value parts into the
    //                         result
    //
    //     format_copy_empty - copy empty elements into the result
    //
    f[".split"] += [](value v, string re, string fmt, optional<names> flags)
    {
      return split (move (v), re, fmt, move (flags));
    };

    f[".split"] += [](value v, names re, names fmt, optional<names> flags)
    {
      return split (move (v),
                    convert<string> (move (re)),
                    convert<string> (move (fmt)),
                    move (flags));
    };

    // $regex.merge(<vals>, <pat>, <fmt> [, <delim> [, <flags>]])
    //
    // Replace matched parts in a list of elements using the regex format
    // string. Convert the elements to strings prior to matching. The result
    // value is untyped and contains concatenation of transformed non-empty
    // elements (unless the `format_copy_empty` flag is specified) optionally
    // separated with a delimiter.
    //
    // The following flags are supported:
    //
    //     icase             - match ignoring case
    //
    //     format_first_only - only replace the first match
    //
    //     format_no_copy    - do not copy unmatched value parts into the
    //                         result
    //
    //     format_copy_empty - copy empty elements into the result
    //
    // If both `format_first_only` and `format_no_copy` flags are specified
    // then the result will be a concatenation of only the first match
    // replacements.
    //
    f[".merge"] += [](names ns,
                      string re,
                      string fmt,
                      optional<string*> delim,
                      optional<names> flags)
    {
      return merge (move (ns),
                    re,
                    fmt,
                    delim && *delim != nullptr
                    ? move (**delim)
                    : optional<string> (),
                    move (flags));
    };

    f[".merge"] += [](names ns,
                      names re,
                      names fmt,
                      optional<names*> delim,
                      optional<names> flags)
    {
      return merge (move (ns),
                    convert<string> (move (re)),
                    convert<string> (move (fmt)),
                    delim && *delim != nullptr
                    ? convert<string> (move (**delim))
                    : optional<string> (),
                    move (flags));
    };

    // $regex.apply(<vals>, <pat>, <fmt> [, <flags>])
    //
    // Replace matched parts of each element in a list using the regex format
    // string. Convert the elements to strings prior to matching. Return a
    // list of transformed elements, omitting the empty ones (unless the
    // `format_copy_empty` flag is specified).
    //
    // The following flags are supported:
    //
    //     icase             - match ignoring case
    //
    //     format_first_only - only replace the first match
    //
    //     format_no_copy    - do not copy unmatched value parts into the
    //                         result
    //
    //     format_copy_empty - copy empty elements into the result
    //
    // If both `format_first_only` and `format_no_copy` flags are specified
    // then the result elements will only contain the replacement of the first
    // match.
    //
    f[".apply"] += [](names ns, string re, string fmt, optional<names> flags)
    {
      return apply (move (ns), re, fmt, move (flags));
    };

    f[".apply"] += [](names ns, names re, names fmt, optional<names> flags)
    {
      return apply (move (ns),
                    convert<string> (move (re)),
                    convert<string> (move (fmt)),
                    move (flags));
    };
  }
}
