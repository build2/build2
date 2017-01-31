// file      : build2/regex.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  template <typename C>
  pair<std::basic_string<C>, bool>
  regex_replace_ex (const std::basic_string<C>& s,
                    const std::basic_regex<C>& re,
                    const std::basic_string<C>& fmt,
                    std::regex_constants::match_flag_type flags)
  {
    using namespace std;

    using string_type = basic_string<C>;
    using str_it      = typename string_type::const_iterator;
    using regex_it    = regex_iterator<str_it>;

    bool first_only ((flags & std::regex_constants::format_first_only) ==
                     std::regex_constants::format_first_only);

    locale cl; // Copy of the global C++ locale.
    string_type r;

    // Beginning of the last unmatched substring.
    //
    str_it ub (s.begin ());

    regex_it b (s.begin (), s.end (), re, flags);
    regex_it e;
    bool match (b != e);

    for (regex_it i (b); i != e; ++i)
    {
      const match_results<str_it>& m (*i);

      // Copy the preceeding unmatched substring, save the beginning of the
      // one that follows.
      //
      r.append (ub, m.prefix ().second);
      ub = m.suffix ().first;

      if (first_only && i != b)
        r.append (m[0].first, m[0].second); // Append matched substring.
      else
      {
        // The standard implementation calls m.format() here. We perform our
        // own formatting.
        //
        // Note that we are using char type literals with the assumption that
        // being ASCII characters they will be properly "widened" to the
        // corresponding literals of the C template parameter type.
        //
        auto digit = [] (C c) -> int
        {
          return c >= '0' && c <= '9' ? c - '0' : -1;
        };

        enum class case_conv {none, upper, lower, upper_once, lower_once}
        mode (case_conv::none);

        auto conv_chr = [&mode, &cl] (C c) -> C
        {
          switch (mode)
          {
          case case_conv::upper_once: mode = case_conv::none;
          case case_conv::upper:      c = toupper (c, cl); break;
          case case_conv::lower_once: mode = case_conv::none;
          case case_conv::lower:      c = tolower (c, cl); break;
          case case_conv::none:       break;
          }
          return c;
        };

        auto append_chr = [&r, &conv_chr] (C c)
        {
          r.push_back (conv_chr (c));
        };

        auto append_str = [&r, &mode, &conv_chr] (str_it b, str_it e)
        {
          // Optimize for the common case.
          //
          if (mode == case_conv::none)
            r.append (b, e);
          else
          {
            for (str_it i (b); i != e; ++i)
              r.push_back (conv_chr (*i));
          }
        };

        size_t n (fmt.size ());
        for (size_t i (0); i < n; ++i)
        {
          C c (fmt[i]);

          switch (c)
          {
          case '$':
            {
              // Check if this is a $-based escape sequence. Interpret it
              // accordingly if that's the case, treat '$' as a regular
              // character otherwise.
              //
              c = fmt[++i]; // '\0' if last.

              switch (c)
              {
              case '$': append_chr (c); break;
              case '&': append_str (m[0].first, m[0].second); break;
              case '`':
                {
                  append_str (m.prefix ().first, m.prefix ().second);
                  break;
                }
              case '\'':
                {
                  append_str (m.suffix ().first, m.suffix ().second);
                  break;
                }
              default:
                {
                  // Check if this is a sub-expression 1-based index ($n or
                  // $nn). Append the matching substring if that's the case.
                  // Treat '$' as a regular character otherwise. Index greater
                  // than the sub-expression count is silently ignored.
                  //
                  int si (digit (c));
                  if (si >= 0)
                  {
                    int d;
                    if ((d = digit (fmt[i + 1])) >= 0) // '\0' if last.
                    {
                      si = si * 10 + d;
                      ++i;
                    }
                  }

                  if (si > 0)
                  {
                    // m[0] refers to the matched substring.
                    //
                    if (static_cast<size_t> (si) < m.size ())
                      append_str (m[si].first, m[si].second);
                  }
                  else
                  {
                    // Not a $-based escape sequence so treat '$' as a
                    // regular character.
                    //
                    --i;
                    append_chr ('$');
                  }

                  break;
                }
              }

              break;
            }
          case '\\':
            {
              c = fmt[++i]; // '\0' if last.

              switch (c)
              {
              case '\\': append_chr (c); break;

              case 'u': mode = case_conv::upper_once; break;
              case 'l': mode = case_conv::lower_once; break;
              case 'U': mode = case_conv::upper;      break;
              case 'L': mode = case_conv::lower;      break;
              case 'E': mode = case_conv::none;       break;
              default:
                {
                  // Check if this is a sub-expression 1-based index. Append
                  // the matching substring if that's the case, Skip '\\'
                  // otherwise. Index greater than the sub-expression count is
                  // silently ignored.
                  //
                  int si (digit (c));
                  if (si > 0)
                  {
                    // m[0] refers to the matched substring.
                    //
                    if (static_cast<size_t> (si) < m.size ())
                      append_str (m[si].first, m[si].second);
                  }
                  else
                    --i;

                  break;
                }
              }

              break;
            }
          default:
            {
              // Append a regular character.
              //
              append_chr (c);
              break;
            }
          }
        }
      }
    }

    r.append (ub, s.end ()); // Append the rightmost non-matched substring.
    return make_pair (move (r), match);
  }
}
