// file      : libbuild2/script/script.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/script/script.hxx>

#include <sstream>

#include <libbuild2/algorithm.hxx> // find_if()

using namespace std;

namespace build2
{
  namespace script
  {
    ostream&
    operator<< (ostream& o, line_type lt)
    {
      const char* s (nullptr);

      switch (lt)
      {
      case line_type::var:       s = "variable"; break;
      case line_type::cmd:       s = "command";  break;
      case line_type::cmd_if:    s = "'if'";     break;
      case line_type::cmd_ifn:   s = "'if!'";    break;
      case line_type::cmd_elif:  s = "'elif'";   break;
      case line_type::cmd_elifn: s = "'elif!'";  break;
      case line_type::cmd_else:  s = "'else'";   break;
      case line_type::cmd_end:   s = "'end'";    break;
      }

      return o << s;
    }

    void
    dump (ostream& os, const string& ind, const lines& ls)
    {
      for (const line& l: ls)
      {
        os << ind;

        // @@ Should be across lines?
        //
        // We will consider mixed quoting as a double quoting since the
        // information is lost and we won't be able to restore the token
        // original representation.
        //
//        char qseq ('\0'); // Can be used as bool.

        for (const replay_token& rt: l.tokens)
        {
          const token& t (rt.token);

          // Left and right quotes (can be used as bool).
          //
          char lq ('\0');
          char rq ('\0');

          /*
          if (t.qtype != quote_type::unquoted)
          {
            auto quote = [&t] ()
            {
              return t.qtype == quote_type::single ? '\'' : '"';
            }

            if (t.qcomp) // Complete quoting.
            {
              // If we are inside quoted token sequence then we do noting.
              // Otherwise we just quote the token not starting a sequence.
              //
              if (!qseq)
              {
                lq = quote ();
                rq = lq;
              }
            }
            else         // Partial quoting.
            {
              if (!qseq)
                lq =

            }
          }
          */
          // @@ Add 2 spaces indentation for if block contents.

          if (t.separated                   &&
              t.type != token_type::newline &&
              &rt != &l.tokens[0])             // Not first in the line.
            os << ' ';

          if (lq) os << lq;
          t.printer (os, t, print_mode::raw);
          if (rq) os << rq;

//          prev_qcomp = t.qcomp;
//          prev_qtype = t.qtype;
        }
      }
    }

    // Quote if empty or contains spaces or any of the special characters.
    // Note that we use single quotes since double quotes still allow
    // expansion.
    //
    // @@ What if it contains single quotes?
    //
    static void
    to_stream_q (ostream& o, const string& s)
    {
      if (s.empty () || s.find_first_of (" |&<>=\\\"") != string::npos)
        o << '\'' << s << '\'';
      else
        o << s;
    };

    void
    to_stream (ostream& o, const command& c, command_to_stream m)
    {
      auto print_path = [&o] (const path& p)
      {
        using build2::operator<<;

        ostringstream s;
        stream_verb (s, stream_verb (o));
        s << p;

        to_stream_q (o, s.str ());
      };

      auto print_redirect = [&o, print_path] (const redirect& r,
                                              const char* prefix)
      {
        o << ' ' << prefix;

        size_t n (string::traits_type::length (prefix));
        assert (n > 0);

        char d (prefix[n - 1]); // Redirect direction.

        switch (r.type)
        {
        case redirect_type::none:  assert (false);   break;
        case redirect_type::pass:  o << '|';         break;
        case redirect_type::null:  o << '-';         break;
        case redirect_type::trace: o << '!';         break;
        case redirect_type::merge: o << '&' << r.fd; break;

        case redirect_type::here_str_literal:
        case redirect_type::here_doc_literal:
          {
            bool doc (r.type == redirect_type::here_doc_literal);

            // For here-document add another '>' or '<'. Note that here end
            // marker never needs to be quoted.
            //
            if (doc)
              o << d;

            o << r.modifiers;

            if (doc)
              o << r.end;
            else
            {
              const string& v (r.str);
              to_stream_q (o,
                           r.modifiers.find (':') == string::npos
                           ? string (v, 0, v.size () - 1) // Strip newline.
                           : v);
            }

            break;
          }

        case redirect_type::here_str_regex:
        case redirect_type::here_doc_regex:
          {
            bool doc (r.type == redirect_type::here_doc_regex);

            // For here-document add another '>' or '<'. Note that here end
            // marker never needs to be quoted.
            //
            if (doc)
              o << d;

            o << r.modifiers;

            const regex_lines& re (r.regex);

            if (doc)
              o << re.intro + r.end + re.intro + re.flags;
            else
            {
              assert (!re.lines.empty ()); // Regex can't be empty.

              regex_line l (re.lines[0]);
              to_stream_q (o, re.intro + l.value + re.intro + l.flags);
            }

            break;
          }

        case redirect_type::file:
          {
            // For stdin or stdout-comparison redirect add '>>' or '<<' (and
            // so make it '<<<' or '>>>'). Otherwise add '+' or '=' (and so
            // make it '>+' or '>=').
            //
            if (d == '<' || r.file.mode == redirect_fmode::compare)
              o << d << d;
            else
              o << (r.file.mode == redirect_fmode::append ? '+' : '=');

            print_path (r.file.path);
            break;
          }

        case redirect_type::here_doc_ref: assert (false); break;
        }
      };

      auto print_doc = [&o] (const redirect& r)
      {
        o << endl;

        if (r.type == redirect_type::here_doc_literal)
          o << r.str;
        else
        {
          assert (r.type == redirect_type::here_doc_regex);

          const regex_lines& rl (r.regex);

          for (auto b (rl.lines.cbegin ()), i (b), e (rl.lines.cend ());
               i != e; ++i)
          {
            if (i != b)
              o << endl;

            const regex_line& l (*i);

            if (l.regex)                  // Regex (possibly empty),
              o << rl.intro << l.value << rl.intro << l.flags;
            else if (!l.special.empty ()) // Special literal.
              o << rl.intro;
            else                          // Textual literal.
              o << l.value;

            o << l.special;
          }
        }

        o << (r.modifiers.find (':') == string::npos ? "" : "\n") << r.end;
      };

      if ((m & command_to_stream::header) == command_to_stream::header)
      {
        // Program.
        //
        to_stream_q (o, c.program.string ());

        // Arguments.
        //
        for (const string& a: c.arguments)
        {
          o << ' ';
          to_stream_q (o, a);
        }

        // Redirects.
        //
        // Print the none redirect (no data allowed) if/when the respective
        // syntax is invened.
        //
        if (c.in && c.in->effective ().type != redirect_type::none)
          print_redirect (c.in->effective (), "<");

        if (c.out && c.out->effective ().type != redirect_type::none)
          print_redirect (c.out->effective (), ">");

        if (c.err && c.err->effective ().type != redirect_type::none)
          print_redirect (c.err->effective (), "2>");

        for (const auto& p: c.cleanups)
        {
          o << " &";

          if (p.type != cleanup_type::always)
            o << (p.type == cleanup_type::maybe ? '?' : '!');

          print_path (p.path);
        }

        if (c.exit.comparison != exit_comparison::eq || c.exit.code != 0)
        {
          switch (c.exit.comparison)
          {
          case exit_comparison::eq: o << " == "; break;
          case exit_comparison::ne: o << " != "; break;
          }

          o << static_cast<uint16_t> (c.exit.code);
        }
      }

      if ((m & command_to_stream::here_doc) == command_to_stream::here_doc)
      {
        // Here-documents.
        //
        if (c.in &&
            (c.in->type == redirect_type::here_doc_literal ||
             c.in->type == redirect_type::here_doc_regex))
          print_doc (*c.in);

        if (c.out &&
            (c.out->type == redirect_type::here_doc_literal ||
             c.out->type == redirect_type::here_doc_regex))
          print_doc (*c.out);

        if (c.err &&
            (c.err->type == redirect_type::here_doc_literal ||
             c.err->type == redirect_type::here_doc_regex))
          print_doc (*c.err);
      }
    }

    void
    to_stream (ostream& o, const command_pipe& p, command_to_stream m)
    {
      if ((m & command_to_stream::header) == command_to_stream::header)
      {
        for (auto b (p.begin ()), i (b); i != p.end (); ++i)
        {
          if (i != b)
            o << " | ";

          to_stream (o, *i, command_to_stream::header);
        }
      }

      if ((m & command_to_stream::here_doc) == command_to_stream::here_doc)
      {
        for (const command& c: p)
          to_stream (o, c, command_to_stream::here_doc);
      }
    }

    void
    to_stream (ostream& o, const command_expr& e, command_to_stream m)
    {
      if ((m & command_to_stream::header) == command_to_stream::header)
      {
        for (auto b (e.begin ()), i (b); i != e.end (); ++i)
        {
          if (i != b)
          {
            switch (i->op)
            {
            case expr_operator::log_or:  o << " || "; break;
            case expr_operator::log_and: o << " && "; break;
            }
          }

          to_stream (o, i->pipe, command_to_stream::header);
        }
      }

      if ((m & command_to_stream::here_doc) == command_to_stream::here_doc)
      {
        for (const expr_term& t: e)
          to_stream (o, t.pipe, command_to_stream::here_doc);
      }
    }

    // redirect
    //
    redirect::
    redirect (redirect_type t)
        : type (t)
    {
      switch (type)
      {
      case redirect_type::none:
      case redirect_type::pass:
      case redirect_type::null:
      case redirect_type::trace:
      case redirect_type::merge: break;

      case redirect_type::here_str_literal:
      case redirect_type::here_doc_literal: new (&str) string (); break;

      case redirect_type::here_str_regex:
      case redirect_type::here_doc_regex:
        {
          new (&regex) regex_lines ();
          break;
        }

      case redirect_type::file: new (&file) file_type (); break;

      case redirect_type::here_doc_ref: assert (false); break;
      }
    }

    redirect::
    redirect (redirect&& r) noexcept
        : type (r.type),
          modifiers (move (r.modifiers)),
          end (move (r.end)),
          end_line (r.end_line),
          end_column (r.end_column)
    {
      switch (type)
      {
      case redirect_type::none:
      case redirect_type::pass:
      case redirect_type::null:
      case redirect_type::trace: break;

      case redirect_type::merge: fd = r.fd; break;

      case redirect_type::here_str_literal:
      case redirect_type::here_doc_literal:
        {
          new (&str) string (move (r.str));
          break;
        }
      case redirect_type::here_str_regex:
      case redirect_type::here_doc_regex:
        {
          new (&regex) regex_lines (move (r.regex));
          break;
        }
      case redirect_type::file:
        {
          new (&file) file_type (move (r.file));
          break;
        }
      case redirect_type::here_doc_ref:
        {
          new (&ref) reference_wrapper<const redirect> (r.ref);
          break;
        }
      }
    }

    redirect& redirect::
    operator= (redirect&& r) noexcept
    {
      if (this != &r)
      {
        this->~redirect ();
        new (this) redirect (move (r)); // Assume noexcept move-constructor.
      }
      return *this;
    }

    redirect::
    ~redirect ()
    {
      switch (type)
      {
      case redirect_type::none:
      case redirect_type::pass:
      case redirect_type::null:
      case redirect_type::trace:
      case redirect_type::merge: break;

      case redirect_type::here_str_literal:
      case redirect_type::here_doc_literal: str.~string (); break;

      case redirect_type::here_str_regex:
      case redirect_type::here_doc_regex: regex.~regex_lines (); break;

      case redirect_type::file: file.~file_type (); break;

      case redirect_type::here_doc_ref:
        {
          ref.~reference_wrapper<const redirect> ();
          break;
        }
      }
    }

    redirect::
    redirect (const redirect& r)
        : type (r.type),
          modifiers (r.modifiers),
          end (r.end),
          end_line (r.end_line),
          end_column (r.end_column)
    {
      switch (type)
      {
      case redirect_type::none:
      case redirect_type::pass:
      case redirect_type::null:
      case redirect_type::trace: break;

      case redirect_type::merge: fd = r.fd; break;

      case redirect_type::here_str_literal:
      case redirect_type::here_doc_literal:
        {
          new (&str) string (r.str);
          break;
        }
      case redirect_type::here_str_regex:
      case redirect_type::here_doc_regex:
        {
          new (&regex) regex_lines (r.regex);
          break;
        }
      case redirect_type::file:
        {
          new (&file) file_type (r.file);
          break;
        }
      case redirect_type::here_doc_ref:
        {
          new (&ref) reference_wrapper<const redirect> (r.ref);
          break;
        }
      }
    }

    redirect& redirect::
    operator= (const redirect& r)
    {
      if (this != &r)
        *this = redirect (r); // Reduce to move-assignment.
      return *this;
    }

    // environment
    //
    void environment::
    clean (script::cleanup c, bool implicit)
    {
      using script::cleanup;

      assert (!implicit || c.type == cleanup_type::always);

      const path& p (c.path);

      if (sandbox_dir.empty () || !p.sub (sandbox_dir))
      {
        if (implicit)
          return;
        else
          assert (false); // Error so should have been checked.
      }

      auto pr = [&p] (const cleanup& v) -> bool {return v.path == p;};
      auto i (find_if (cleanups.begin (), cleanups.end (), pr));

      if (i == cleanups.end ())
        cleanups.emplace_back (move (c));
      else if (!implicit)
        i->type = c.type;
    }

    void environment::
    clean_special (path p)
    {
      special_cleanups.emplace_back (move (p));
    }
  }
}
