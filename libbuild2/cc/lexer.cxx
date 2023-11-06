// file      : libbuild2/cc/lexer.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/lexer.hxx>

using namespace std;
using namespace butl;

// bit 0 - identifier character (_0-9A-Ba-b).
//
static const uint8_t char_flags[256] =
//0    1    2    3    4    5    6    7      8    9    A    B    C    D    E    F
{
  0,   0,   0,   0,   0,   0,   0,   0,     0,   0,   0,   0,   0,   0,   0,   0, // 0
  0,   0,   0,   0,   0,   0,   0,   0,     0,   0,   0,   0,   0,   0,   0,   0, // 1
  0,   0,   0,   0,   0,   0,   0,   0,     0,   0,   0,   0,   0,   0,   0,   0, // 2
  1,   1,   1,   1,   1,   1,   1,   1,     1,   1,   0,   0,   0,   0,   0,   0, // 3
  0,   1,   1,   1,   1,   1,   1,   1,     1,   1,   1,   1,   1,   1,   1,   1, // 4
  1,   1,   1,   1,   1,   1,   1,   1,     1,   1,   1,   0,   0,   0,   0,   1, // 5
  0,   1,   1,   1,   1,   1,   1,   1,     1,   1,   1,   1,   1,   1,   1,   1, // 6
  1,   1,   1,   1,   1,   1,   1,   1,     1,   1,   1,   0,   0,   0,   0,   0, // 7

  // 128-255
  0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0
};

// Diagnostics plumbing.
//
namespace butl // ADL
{
  inline build2::location
  get_location (const butl::char_scanner<>::xchar& c, const void* data)
  {
    using namespace build2;

    assert (data != nullptr); // E.g., must be &lexer::name_.
    return location (*static_cast<const path_name*> (data), c.line, c.column);
  }
}

namespace build2
{
  namespace cc
  {
    auto lexer::
    peek (bool e) -> xchar
    {
      if (ungetn_ != 0)
        return ungetb_[ungetn_ - 1];

      if (unpeek_)
        return unpeekc_;

      xchar c (base::peek ());

      if (e && c == '\\')
      {
        get (c);
        xchar p (base::peek ());

        // Handle Windows CRLF sequence. Similar to char_scanner, we treat a
        // single CR as if it was followed by LF and also collapse multiple
        // CRs.
        //
        while (p == '\r')
        {
          get (p);
          p = base::peek ();

          if (p == '\n')
            break;

          // Pretend '\n' was there and recurse.
          //
          if (p != '\r')
            return peek (e);
        }

        if (p == '\n')
        {
          get (p);
          return peek (e); // Recurse.
        }

        // Save in the unpeek buffer so that it is returned on the subsequent
        // calls to peek() (until get()).
        //
        unpeek_ = true;
        unpeekc_ = c;
      }

      return c;
    }

    inline auto lexer::
    get (bool e) -> xchar
    {
      if (ungetn_ != 0)
        return ungetb_[--ungetn_];
      else
      {
        xchar c (peek (e));
        get (c);
        return c;
      }
    }

    inline void lexer::
    get (const xchar& c)
    {
      // Increment the logical line similar to how base will increment the
      // physical (the column counts are the same).
      //
      if (log_line_ && c == '\n' && ungetn_ == 0)
        ++*log_line_;

      base::get (c);
    }

    inline auto lexer::
    geth (bool e) -> xchar
    {
      xchar c (get (e));
      cs_.append (c);
      return c;
    }

    inline void lexer::
    geth (const xchar& c)
    {
      get (c);
      cs_.append (c);
    }

    using type = token_type;

    void lexer::
    next (token& t, pair<xchar, bool> cf, bool ignore_pp)
    {
      for (;; cf = skip_spaces ())
      {
        xchar c (cf.first);

        t.first = cf.second;
        t.file = &log_file_;
        t.line = log_line_ ? *log_line_ : c.line;
        t.column = c.column;

        if (eos (c))
        {
          t.type = type::eos;
          return;
        }

        const location l (name_, c.line, c.column);

        // Hash the token's line. The reason is debug info. In fact, doing
        // this will make quite a few "noop" changes (like adding a newline
        // anywhere in the source) cause the checksum change. But there
        // doesn't seem to be any way around it: the case where we benefit
        // from the precise change detection the most (development) is also
        // where we will most likely have debug info enable.
        //
        // Note that in order not to make this completely useless we don't
        // hash the column. Even if it is part of the debug info, having it a
        // bit off shouldn't cause any significant mis-positioning. We also
        // don't hash the file path for each token instead only hashing it
        // when changed with the #line directive (as well as in the
        // constructor for the initial path).
        //
        cs_.append (t.line);
        cs_.append (c);

        switch (c)
        {
          // Preprocessor lines.
          //
        case '#':
          {
            // It is tempting to simply scan until the newline ignoring
            // anything in between. However, these lines can start a
            // multi-line C-style comment. So we have to tokenize them (and
            // hash the data for each token).
            //
            // Note that this may not work for things like #error that can
            // contain pretty much anything. Also note that lines that start
            // with '#' can contain '#' further down. In this case we need to
            // be careful not to recurse (and consume multiple newlines). Thus
            // the ignore_pp flag.
            //
            // Finally, to support diagnostics properly we need to recognize
            // #line directives.
            //
            if (ignore_pp)
            {
              for (bool first (true);;)
              {
                // Note that we keep using the passed token for buffers.
                //
                c = skip_spaces (false).first; // Stop at newline.

                if (eos (c) || c == '\n')
                  break;

                if (first)
                {
                  first = false;

                  // Recognize #line and its shorthand version:
                  //
                  // #line <integer> [<string literal>] ...
                  // #     <integer> [<string literal>] ...
                  //
                  // Also diagnose #include while at it if preprocessed.
                  //
                  if (!(c >= '0' && c <= '9'))
                  {
                    next (t, make_pair (c, false), false);

                    if (t.type == type::identifier)
                    {
                      if (t.value != "line")
                      {
                        if (preprocessed_ && t.value == "include")
                          fail (l) << "unexpected #include directive";

                        continue;
                      }
                    }
                    else
                      continue;

                    if (t.type != type::identifier || t.value != "line")
                      continue;

                    c = skip_spaces (false).first;

                    if (!(c >= '0' && c <= '9'))
                      fail (c) << "line number expected after #line directive";
                  }

                  // Ok, this is #line and next comes the line number.
                  //
                  line_directive (t, c);
                  continue; // Parse the tail, if any.
                }

                next (t, make_pair (c, false), false);
              }
              break;
            }
            else
            {
              t.type = type::punctuation;
              return;
            }
          }
          // Single-letter punctuation.
          //
        case ';': t.type = type::semi;    return;
        case '{': t.type = type::lcbrace; return;
        case '}': t.type = type::rcbrace; return;
          // Other single-letter punctuation.
          //
        case '(':
        case ')':
        case '[':
        case ']':
        case ',':
        case '?':
        case '~':
        case '\\': t.type = type::punctuation; return;
          // Potentially multi-letter punctuation.
          //
        case '.': // . .* .<N> ...
          {
            xchar p (peek ());

            if (p == '*')
            {
              geth (p);
              t.type = type::punctuation;
              return;
            }
            else if (p >= '0' && p <= '9')
            {
              number_literal (t, c);
              return;
            }
            else if (p == '.')
            {
              get (p);

              xchar q (peek ());
              if (q == '.')
              {
                cs_.append (p);

                geth (q);
                t.type = type::punctuation;
                return;
              }
              unget (p);
              // Fall through.
            }

            t.type = type::dot;
            return;
          }
        case '=': // = ==
        case '!': // ! !=
        case '*': // * *=
        case '/': // / /=   (/* and // handled by skip_spaced() above)
        case '%': // % %=
        case '^': // ^ ^=
          {
            xchar p (peek ());

            if (p == '=')
              geth (p);

            t.type = type::punctuation;
            return;
          }
        case '<': // < <= << <<=
        case '>': // > >= >> >>=
          {
            xchar p (peek ());

            if (p == c)
            {
              geth (p);
              if ((p = peek ()) == '=')
                geth (p);
              t.type = type::punctuation;
            }
            else if (p == '=')
            {
              geth (p);
              t.type = type::punctuation;
            }
            else
              t.type = (c == '<' ? type::less : type::greater);

            return;
          }
        case '+': // + ++ +=
        case '-': // - -- -= -> ->*
          {
            xchar p (peek ());

            if (p == c || p == '=')
              geth (p);
            else if (c == '-' && p == '>')
            {
              geth (p);
              if ((p = peek ()) == '*')
                geth (p);
            }

            t.type = type::punctuation;
            return;
          }
        case '&': // & && &=
        case '|': // | || |=
          {
            xchar p (peek ());

            if (p == c || p == '=')
              geth (p);

            t.type = type::punctuation;
            return;
          }
        case ':': // : ::
          {
            xchar p (peek ());

            if (p == ':')
            {
              geth (p);
              t.type = type::scope;
            }
            else
              t.type = type::colon;

            return;
          }
          // Number (and also .<N> above).
          //
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
          {
            number_literal (t, c);
            return;
          }
          // Char/string literal, identifier, or other (\, $, @, `).
          //
        default:
          {
            bool raw (false); // Raw string literal.

            // Note: known not to be a digit (see above).
            //
            if (char_flags[static_cast<uint8_t> (c)] & 0x01)
            {
              // This smells a little: we know skip_spaces() did not peek at
              // the next character because this is not '/'. Which means the
              // position in the stream must be of this character + 1.
              //
              t.position = buf_->tellg () - 1;

              string& id (t.value);
              id = c;

              while (char_flags[static_cast<uint8_t> (c = peek ())] & 0x01)
              {
                geth (c);
                id += c;

                // Direct buffer scan. Note that we always follow up with the
                // normal peek() call which may load the next chunk, handle
                // line continuations, etc. In other words, the end of the
                // "raw" scan doesn't necessarily mean the end.
                //
                const char* b (gptr_);
                const char* p (b);

                for (const char* e (egptr_);
                     p != e && char_flags[static_cast<uint8_t> (*p)] & 0x01;
                     ++p) ;

                // Unrolling this loop doesn't make a difference.
                //
                // for (const char* e (egptr_ - 4); p < e; p += 4)
                // {
                //   uint8_t c;
                //
                //  c = static_cast<uint8_t> (p[0]);
                //  if (!(char_flags[c] & 0x01)) break;
                //
                //  c = static_cast<uint8_t> (p[1]);
                //  if (!(char_flags[c] & 0x01)) {p += 1; break;}
                //
                //  c = static_cast<uint8_t> (p[2]);
                //  if (!(char_flags[c] & 0x01)) {p += 2; break;}
                //
                //  c = static_cast<uint8_t> (p[3]);
                //  if (!(char_flags[c] & 0x01)) {p += 3; break;}
                // }

                size_t n (p - b);
                id.append (b, n); cs_.append (b, n);
                gptr_ = p; buf_->gbump (static_cast<int> (n)); column += n;
              }

              // If the following character is a quote, see if the identifier
              // is one of the literal prefixes.
              //
              if (c == '\'' || c == '\"')
              {
                size_t n (id.size ()), i (0);
                switch (id[0])
                {
                case 'u':
                  {
                    if (n > 1 && id[1] == '8')
                      ++i;
                  }
                  // Fall through.
                case 'L':
                case 'U':
                  {
                    ++i;

                    if (c == '\"' && n > i && id[i] == 'R')
                    {
                      ++i;
                      raw = true;
                    }
                    break;
                  }
                case 'R':
                  {
                    if (c == '\"')
                    {
                      ++i;
                      raw = true;
                    }
                    break;
                  }
                }

                if (i == n) // All characters "consumed".
                {
                  geth (c);
                  id.clear ();
                }
              }

              if (!id.empty ())
              {
                t.type = type::identifier;
                return;
              }
            }

            switch (c)
            {
            case '\'':
              {
                char_literal (t, c);
                return;
              }
            case '\"':
              {
                if (raw)
                  raw_string_literal (t, c);
                else
                  string_literal (t, c);
                return;
              }
            default:
              {
                t.type = type::other;
                return;
              }
            }
          }
        }
      }
    }

    void lexer::
    number_literal (token& t, xchar c)
    {
      // note: c is hashed

      // A number (integer or floating point literal) can:
      //
      // 1. Start with a dot (which must be followed by a digit, e.g., .123).
      //
      // 2. Can have a radix prefix (0b101, 0123, 0X12AB).
      //
      // 3. Can have an exponent (1e10, 0x1.p-10, 1.).
      //
      // 4. Digits can be separated with ' (123'456, 0xff00'00ff).
      //
      // 5. End with a built-in or user defined literal (123f, 123UL, 123_X)
      //
      // Quoting from GCC's preprocessor documentation:
      //
      // "Formally preprocessing numbers begin with an optional period, a
      // required decimal digit, and then continue with any sequence of
      // letters, digits, underscores, periods, and exponents. Exponents are
      // the two-character sequences 'e+', 'e-', 'E+', 'E-', 'p+', 'p-', 'P+',
      // and 'P-'."
      //
      // So it looks like a "C++ number" is then any unseparated (with
      // whitespace or punctuation) sequence of those plus '. The only mildly
      // tricky part is then to recognize +/- as being part of the exponent.
      //
      while (!eos ((c = peek ())))
      {
        switch (c)
        {
          // All the whitespace, punctuation, and other characters that end
          // the number.
          //
        case ' ':
        case '\n':
        case '\t':
        case '\r':
        case '\f':
        case '\v':

        case '#':
        case ';':
        case '{':
        case '}':
        case '(':
        case ')':
        case '[':
        case ']':
        case ',':
        case '?':
        case '~':
        case '=':
        case '!':
        case '*':
        case '/':
        case '%':
        case '^':
        case '>':
        case '<':
        case '&':
        case '|':
        case ':':
        case '+': // The exponent case is handled below.
        case '-': // The exponent case is handled below.
        case '"':
        case '\\':

        case '@':
        case '$':
        case '`':
          break;

          // Recognize +/- after the exponent.
          //
        case 'e':
        case 'E':
        case 'p':
        case 'P':
          {
            geth (c);
            c = peek ();
            if (c == '+' || c == '-')
              geth (c);
            continue;
          }

        case '_':
        case '.':
        case '\'':
        default: // Digits and letters.
          {
            geth (c);
            continue;
          }
        }

        break;
      }

      t.type = type::number;
    }

    void lexer::
    char_literal (token& t, xchar c)
    {
      // note: c is hashed

      const location l (name_, c.line, c.column);

      for (char p (c);;) // Previous character (see below).
      {
        c = geth ();

        if (eos (c) || c == '\n')
          fail (l) << "unterminated character literal";

        if (c == '\'' && p != '\\')
          break;

        // Keep track of \\-escapings so we don't confuse them with \', as in
        // '\\'.
        //
        p = (c == '\\' && p == '\\') ? '\0' : static_cast<char> (c);
      }

      // See if we have a user-defined suffix (which is an identifier).
      //
      if ((c = peek ()) == '_' || alpha (c))
        literal_suffix (c);

      t.type = type::character;
    }

    void lexer::
    string_literal (token& t, xchar c)
    {
      // note: c is hashed

      const location l (name_, c.line, c.column);

      for (char p (c);;) // Previous character (see below).
      {
        c = geth ();

        if (eos (c) || c == '\n')
          fail (l) << "unterminated string literal";

        if (c == '\"' && p != '\\')
          break;

        // Keep track of \\-escapings so we don't confuse them with \", as in
        // "\\".
        //
        p = (c == '\\' && p == '\\') ? '\0' : static_cast<char> (c);

        // Direct buffer scan.
        //
        if (p != '\\')
        {
          const char* b (gptr_);
          const char* e (egptr_);
          const char* p (b);

          for (char c;
               p != e &&
               (c = *p) != '\"' && c != '\\' && c != '\n' && c != '\r';
               ++p) ;

          size_t n (p - b);
          cs_.append (b, n);
          gptr_ = p; buf_->gbump (static_cast<int> (n)); column += n;
        }
      }

      // See if we have a user-defined suffix (which is an identifier).
      //
      if ((c = peek ()) == '_' || alpha (c))
        literal_suffix (c);

      t.type = type::string;
    }

    void lexer::
    raw_string_literal (token& t, xchar c)
    {
      // note: c is hashed

      // The overall form is:
      //
      // R"<delimiter>(<raw_characters>)<delimiter>"
      //
      // Where <delimiter> is a potentially-empty character sequence made of
      // any source character but parentheses, backslash, and spaces (in
      // particular, it can be `"`). It can be at most 16 characters long.
      //
      // Note that the <raw_characters> are not processed in any way, not even
      // for line continuations.
      //
      const location l (name_, c.line, c.column);

      // As a first step, parse the delimiter (including the openning paren).
      //
      string d (1, ')');

      for (;;)
      {
        c = geth ();

        if (eos (c) || c == ')' || c == '\\' || c == ' ')
          fail (l) << "invalid raw string literal";

        if (c == '(')
          break;

        d += c;
      }

      d += '"';

      // Now parse the raw characters while trying to match the closing
      // delimiter.
      //
      for (size_t i (0);;) // Position to match in d.
      {
        c = geth (false); // No newline escaping.

        if (eos (c)) // Note: newline is ok.
          fail (l) << "invalid raw string literal";

        if (c != d[i] && i != 0) // Restart from the beginning.
          i = 0;

        if (c == d[i])
        {
          if (++i == d.size ())
            break;
        }
      }

      // See if we have a user-defined suffix (which is an identifier).
      //
      if ((c = peek ()) == '_' || alpha (c))
        literal_suffix (c);

      t.type = type::string;
    }

    void lexer::
    literal_suffix (xchar c)
    {
      // note: c is unhashed

      // Parse a user-defined literal suffix identifier.
      //
      for (geth (c); (c = peek ()) == '_' || alnum (c); geth (c)) ;
    }

    void lexer::
    line_directive (token& t, xchar c)
    {
      // enter: first digit of the line number
      // leave: last character of the line number or file string
      // note:  c is unhashed

      // If our number and string tokens contained the literal values, then we
      // could have used that. However, we ignore the value (along with escape
      // processing, etc), for performance. Let's keep it that way and instead
      // handle it ourselves.
      //
      // Note also that we are not hashing these at the character level
      // instead hashing the switch to a new file path below and leaving the
      // line number to the token line hashing.
      //
      {
        string& s (t.value);

        for (s = c; (c = peek ()) >= '0' && c <= '9'; get (c))
          s += c;

        // The newline that ends the directive will increment the logical line
        // so subtract one to compensate. Note: can't be 0 and shouldn't throw
        // for valid lines.
        //
        log_line_ = stoull (s.c_str ()) - 1;
      }

      // See if we have the file.
      //
      c = skip_spaces (false).first;

      if (c == '\"')
      {
        const location l (name_, c.line, c.column);

        // It is common to have a large number of #line directives that don't
        // change the file (they seem to be used to track macro locations or
        // some such). So we are going to optimize for this by comparing the
        // current path to what's in #line.
        //
        string& s (tmp_file_);
        s.clear ();

        for (char p ('\0'); p != '\"'; ) // Previous character.
        {
          c = get ();

          if (eos (c) || c == '\n')
            fail (l) << "unterminated string literal";

          // Handle escapes.
          //
          if (p == '\\')
          {
            p = '\0'; // Clear so we don't confuse \" and \\".

            // We only handle what can reasonably be expected in a file name.
            //
            switch (c)
            {
            case '\\':
            case '\'':
            case '\"': break; // Add as is.
            default:
              fail (c) << "unsupported escape sequence in #line directive";
            }
          }
          else
          {
            p = c;

            switch (c)
            {
            case '\\':
            case '\"': continue;
            }
          }

          s += c;

          // Direct buffer scan.
          //
          if (p != '\\')
          {
            const char* b (gptr_);
            const char* e (egptr_);
            const char* p (b);

            for (char c;
                 p != e &&
                 (c = *p) != '\"' && c != '\\' && c != '\n' && c != '\r';
                 ++p) ;

            size_t n (p - b);
            s.append (b, n);
            gptr_ = p; buf_->gbump (static_cast<int> (n)); column += n;
          }
        }

        try
        {
          if (s.empty ())
            throw invalid_path ("");

          // Handle special names (<stdin>, <built-in>, etc).
          //
          if (s.front () == '<' && s.back () == '>')
          {
            if (log_file_.name)
            {
              if (*log_file_.name == s)
                return;

              log_file_.name->swap (s);
            }
            else
              log_file_.name = move (s);

            log_file_.path.clear ();
          }
          else
          {
            if (log_file_.path.string () == s)
              return;

            string r (move (log_file_.path).string ()); // Move string rep out.
            r.swap (s);
            log_file_.path = path (move (r)); // Move back in.

            log_file_.name = nullopt;
          }
        }
        catch (const invalid_path&)
        {
          fail (l) << "invalid path in #line directive";
        }

        // If the path is relative, then prefix it with the current working
        // directory. Failed that, we will end up with different checksums for
        // invocations from different directories.
        //
        // While this should work fine for normal cross-compilation, it's an
        // entirely different story for the emulated case (e.g., msvc-linux
        // where the preprocessed output contains absolute Windows paths). So
        // we try to sense if things look fishy and leave the path alone.
        //
        // Also detect special names like <built-in> and <command-line>. Plus
        // GCC sometimes adds what looks like working directory (has trailing
        // slash). So ignore that as well.
        //
        // We now switched to using absolute translation unit paths (because
        // of __FILE__/assert(); see compile.cxx for details). But we might
        // still need this logic when we try to calculate location-independent
        // hash for distributed compilation/caching. The idea is to only hash
        // the part starting from the project root which is immutable. Plus
        // we will need -ffile-prefix-map to deal with __FILE__.
        //
        if (!log_file_.path.to_directory ()) // Also covers special names.
          cs_.append (log_file_.name
                      ? *log_file_.name
                      : log_file_.path.string ());
#if 0
        {
          using tr = path::traits_type;
          const string& f (log_file_.path.string ());

          if (log_file_.name               ||
              f.find (':') != string::npos ||
              log_file_.path.absolute ())
            cs_.append (log_file_.name ? *log_file_.name : f);
          else
          {
            // This gets complicated and slow: the path may contain '..' and
            // '.'  so strictly speaking we would need to normalize it.
            // Instead, we are going to handle leading '..'s ourselves (the
            // sane case) and ignore everything else (so if you have '..'  or
            // '.' somewhere in the middle, then things might not work
            // optimally for you).
            //
            const string& d (work.string ());

            // Iterate over leading '..' in f "popping" the corresponding
            // number of trailing components from d.
            //
            size_t fp (0);
            size_t dp (d.size () - 1);

            for (size_t p;; )
            {
              // Note that in file we recognize any directory separator, not
              // just of this platform (see note about emulation above).
              //
              if (f.compare (fp, 2, "..") != 0  ||
                  (f[fp + 2] != '/' && f[fp + 2] != '\\') || // Could be '\0'.
                  (p = tr::rfind_separator (d, dp)) == string::npos)
                break;

              fp += 3;
              dp = p - 1;
            }

            cs_.append (d.c_str (), dp + 1);
            cs_.append (tr::directory_separator); // Canonical in work.
            cs_.append (f.c_str () + fp);
          }
        }
#endif
      }
      else
        unget (c);
    }

    auto lexer::
    skip_spaces (bool nl) -> pair<xchar, bool>
    {
      xchar c (get ());

      // Besides the first character, we also need to take into account any
      // newlines that we are skipping. For example, the first character may
      // be a space at the end of the line which we will skip along with the
      // following newline.
      //
      bool first (c.column == 1);

      for (; !eos (c); c = get ())
      {
        switch (c)
        {
        case '\n':
          if (!nl) break;
          first = true;
          // Fall through.
        case ' ':
        case '\t':
        case '\r':
        case '\f':
        case '\v':
          {
            // Direct buffer scan.
            //
            const char* b (gptr_);
            const char* e (egptr_);
            const char* p (b);

            for (char c;
                 p != e && ((c = *p) == ' ' || c == '\t');
                 ++p) ;

            size_t n (p - b);
            gptr_ = p; buf_->gbump (static_cast<int> (n)); column += n;

            continue;
          }
        case '/':
          {
            xchar p (peek ());

            // C++ comment.
            //
            if (p == '/')
            {
              get (p);

              for (;;)
              {
                c = get ();
                if (c == '\n' || eos (c))
                  break;

                // Direct buffer scan.
                //
                const char* b (gptr_);
                const char* e (egptr_);
                const char* p (b);

                for (char c;
                     p != e && (c = *p) != '\n' && c != '\\';
                     ++p) ;

                size_t n (p - b);
                gptr_ = p; buf_->gbump (static_cast<int> (n)); column += n;
              }

              if (!nl)
                break;

              first = true;
              continue;
            }

            // C comment.
            //
            // Note that for the first logic we consider a C comment to be
            // entirely part of the same logical line even if there are
            // newlines inside.
            //
            if (p == '*')
            {
              get (p);

              for (;;)
              {
                c = get ();

                if (eos (c))
                  fail (p) << "unterminated comment";

                if (c == '*')
                {
                  if ((c = peek ()) == '/')
                  {
                    get (c);
                    break;
                  }
                }
                else
                {
                  // Direct buffer scan.
                  //
                  const char* b (gptr_);
                  const char* e (egptr_);
                  const char* p (b);

                  for (char c;
                       p != e && (c = *p) != '*' && c != '\\';
                       ++p)
                  {
                    if (c == '\n')
                    {
                      if (log_line_) ++*log_line_;
                      ++line;
                      column = 1;
                    }
                    else
                      ++column;
                  }

                  gptr_ = p; buf_->gbump (static_cast<int> (p - b));
                }
              }
              continue;
            }
            break;
          }
        }
        break;
      }

      return make_pair (c, first);
    }

    ostream&
    operator<< (ostream& o, const token& t)
    {
      switch (t.type)
      {
      case type::dot:         o << "'.'";                   break;
      case type::semi:        o << "';'";                   break;
      case type::colon:       o << "':'";                   break;
      case type::scope:       o << "'::'";                  break;
      case type::less:        o << "'<'";                   break;
      case type::greater:     o << "'>'";                   break;
      case type::lcbrace:     o << "'{'";                   break;
      case type::rcbrace:     o << "'}'";                   break;
      case type::punctuation: o << "<punctuation>";         break;

      case type::identifier:  o << '\'' << t.value << '\''; break;

      case type::number:      o << "<number literal>";      break;
      case type::character:   o << "<char literal>";        break;
      case type::string:      o << "<string literal>";      break;

      case type::other:       o << "<other>";               break;
      case type::eos:         o << "<end of file>";         break;
      }

      return o;
    }
  }
}
