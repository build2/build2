// file      : libbuild2/script/script.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/script/script.hxx>

#include <chrono>
#include <sstream>
#include <cstring> // strchr()

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
      case line_type::var:            s = "variable"; break;
      case line_type::cmd:            s = "command";  break;
      case line_type::cmd_if:         s = "'if'";     break;
      case line_type::cmd_ifn:        s = "'if!'";    break;
      case line_type::cmd_elif:       s = "'elif'";   break;
      case line_type::cmd_elifn:      s = "'elif!'";  break;
      case line_type::cmd_else:       s = "'else'";   break;
      case line_type::cmd_while:      s = "'while'";  break;
      case line_type::cmd_for_args:   s = "'for'";    break;
      case line_type::cmd_for_stream: s = "'for'";    break;
      case line_type::cmd_end:        s = "'end'";    break;
      }

      return o << s;
    }

    void
    dump (ostream& os, const line& ln, bool newline)
    {
      // Print the line's tokens literal representation trying to reproduce
      // the quoting. Consider mixed quoting as double quoting since the
      // information is lost.
      //
      const replay_tokens& rts (ln.tokens);

      assert (!rts.empty ());         // ... <newline>
      const token& ft (rts[0].token);

      // If true, this is a special builtin line.
      //
      // Note that special characters set differs for such lines since they
      // are parsed in the value lexer mode.
      //
      bool builtin (ln.type == line_type::cmd   &&
                    ft.type == token_type::word &&
                    (ft.value == "diag" || ft.value == "depdb"));

      // '"' or '\'' if we are inside the quoted token sequence and '\0'
      // otherwise. Thus, can be used as bool.
      //
      char qseq ('\0');

      optional<token_type> prev_tt;
      for (const replay_token& rt: rts)
      {
        const token& t (rt.token);

        // '"' or '\'' if the token is quoted and '\0' otherwise. Thus, can be
        // used as bool.
        //
        char qtok ('\0');

        switch (t.qtype)
        {
        case quote_type::unquoted: qtok = '\0'; break;
        case quote_type::single:   qtok = '\''; break;
        case quote_type::mixed:
        case quote_type::double_:  qtok = '"';  break;
        }

        // If being inside a quoted token sequence we have reached a token
        // quoted differently or the newline, then we probably made a mistake
        // misinterpreting some previous partially quoted token, for example
        // f"oo" as "foo. If that's the case, all we can do is to end the
        // sequence adding the trailing quote.
        //
        // Note that a token inside the quoted sequence may well be unquoted,
        // so for example "$foo" is lexed as:
        //
        //   token  quoting  complete  notes
        //   ''     "        no
        //   $      "        yes
        //   'foo'                     Unquoted since lexed in variable mode.
        //   ''     "        no
        //   \n
        //
        if (qseq &&
            ((qtok && qtok != qseq) || t.type == token_type::newline))
        {
          os << qseq;
          qseq = '\0';
        }

        // Left and right token quotes (can be used as bool).
        //
        char lq ('\0');
        char rq ('\0');

        // If the token is quoted, then determine if/which quotes should be
        // present on its sides and track the quoted token sequence.
        //
        if (qtok)
        {
          if (t.qcomp) // Complete token quoting.
          {
            // If we are inside a quoted token sequence then do noting.
            // Otherwise just quote the current token not starting a sequence.
            //
            if (!qseq)
            {
              lq = qtok;
              rq = qtok;
            }
          }
          else         // Partial token quoting.
          {
            // Note that we can not always reproduce the original tokens
            // representation for partial quoting. For example, the two
            // following tokens are lexed into the identical token objects:
            //
            // "foo
            // f"oo"
            //
            // We will always assume that the partially quoted token either
            // starts or ends the quoted token sequence. Sometimes this ends
            // up unexpectedly, but seems there is not much we can do:
            //
            // f"oo" "ba"r  ->  "foo bar"
            //
            if (!qseq)     // Start quoted sequence.
            {
              lq = qtok;
              qseq = qtok;
            }
            else           // End quoted sequence.
            {
              rq = qtok;
              qseq = '\0';
            }
          }
        }

        // Print the space character prior to the separated token, unless it
        // is a first like token or the newline.
        //
        if (t.separated && t.type != token_type::newline && &rt != &rts[0])
          os << ' ';

        if (lq) os << lq; // Print the left quote, if required.

        // Escape the special characters, unless the token in not a word, is a
        // variable name, or is single-quoted. Note that the special
        // characters set depends on whether the word is double-quoted or
        // unquoted and whether this is a special builtin line or not.
        //
        if (t.type == token_type::word &&
            qtok != '\''               &&
            (!prev_tt || *prev_tt != token_type::dollar))
        {
          for (char c: t.value)
          {
            if (strchr (qtok || builtin ? "\\\"" : "|&<>=\\\"", c) != nullptr)
              os << '\\';

            os << c;
          }
        }
        else if (t.type != token_type::newline || newline)
          t.printer (os, t, print_mode::raw);

        if (rq) os << rq; // Print the right quote, if required.

        prev_tt = t.type;
      }
    }

    void
    dump (ostream& os, const string& ind, const lines& ls)
    {
      // Additionally indent the flow control construct block lines.
      //
      string fc_ind;

      for (const line& l: ls)
      {
        // Before printing indentation, decrease it if the else, end, etc line
        // is reached.
        //
        switch (l.type)
        {
        case line_type::cmd_elif:
        case line_type::cmd_elifn:
        case line_type::cmd_else:
        case line_type::cmd_end:
          {
            size_t n (fc_ind.size ());
            assert (n >= 2);
            fc_ind.resize (n - 2);
            break;
          }
        default: break;
        }

        // Print indentations.
        //
        os << ind << fc_ind;

        // After printing indentation, increase it for the flow control
        // construct block lines.
        //
        switch (l.type)
        {
        case line_type::cmd_if:
        case line_type::cmd_ifn:
        case line_type::cmd_elif:
        case line_type::cmd_elifn:
        case line_type::cmd_else:
        case line_type::cmd_while:
        case line_type::cmd_for_args:
        case line_type::cmd_for_stream: fc_ind += "  "; break;
        default: break;
        }

        dump (os, l, true /* newline */);
      }
    }

    // Quote a string unconditionally, assuming it contains some special
    // characters.
    //
    // If the quote character is present in the string then it is double
    // quoted rather than single quoted. In this case the following characters
    // are escaped:
    //
    // \"
    //
    static void
    to_stream_quoted (ostream& o, const char* s)
    {
      if (strchr (s, '\'') != nullptr)
      {
        o << '"';

        for (; *s != '\0'; ++s)
        {
          // Escape characters special inside double quotes.
          //
          if (strchr ("\\\"", *s) != nullptr)
            o << '\\';

          o << *s;
        }

        o << '"';
      }
      else
        o << '\'' << s << '\'';
    }

    static inline void
    to_stream_quoted (ostream& o, const string& s)
    {
      to_stream_quoted (o, s.c_str ());
    }

    // Quote if empty or contains spaces or any of the command line special
    // characters.
    //
    static void
    to_stream_q (ostream& o, const string& s)
    {
      // NOTE: update dump(line) if adding any new special character.
      //
      if (s.empty () || s.find_first_of (" |&<>=\\\"'") != string::npos)
        to_stream_quoted (o, s);
      else
        o << s;
    }

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

      auto print_redirect = [&o, print_path] (const redirect& r, int fd)
      {
        const redirect& er (r.effective ());

        // Print the none redirect (no data allowed) if/when the respective
        // syntax is invented.
        //
        if (er.type == redirect_type::none)
          return;

        o << ' ';

        // Print the redirect file descriptor.
        //
        if (fd == 2)
          o << fd;

        // Print the redirect original representation and the modifiers, if
        // present.
        //
        r.token.printer (o, r.token, print_mode::raw);

        // Print the rest of the redirect (file path, etc).
        //
        switch (er.type)
        {
        case redirect_type::none:         assert (false); break;
        case redirect_type::here_doc_ref: assert (false); break;

        case redirect_type::pass:
        case redirect_type::null:
        case redirect_type::trace:             break;
        case redirect_type::merge: o << er.fd; break;

        case redirect_type::file:
          {
            print_path (er.file.path);
            break;
          }

        case redirect_type::here_str_literal:
        case redirect_type::here_doc_literal:
          {
            if (er.type == redirect_type::here_doc_literal)
              o << er.end;
            else
            {
              const string& v (er.str);
              to_stream_q (o,
                           er.modifiers ().find (':') == string::npos
                           ? string (v, 0, v.size () - 1) // Strip newline.
                           : v);
            }

            break;
          }

        case redirect_type::here_str_regex:
        case redirect_type::here_doc_regex:
          {
            const regex_lines& re (er.regex);

            if (er.type == redirect_type::here_doc_regex)
              o << re.intro + er.end + re.intro + re.flags;
            else
            {
              assert (!re.lines.empty ()); // Regex can't be empty.

              regex_line l (re.lines[0]);
              to_stream_q (o, re.intro + l.value + re.intro + l.flags);
            }

            break;
          }
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

        o << (r.modifiers ().find (':') == string::npos ? "" : "\n") << r.end;
      };

      if ((m & command_to_stream::header) == command_to_stream::header)
      {
        // Print the env builtin if any of its options/arguments are present.
        //
        if (c.timeout || c.cwd || !c.variables.empty ())
        {
          o << "env";

          // Timeout.
          //
          if (c.timeout)
          {
            o << " -t "
              << chrono::duration_cast<chrono::seconds> (*c.timeout).count ();

            if (c.timeout_success)
              o << " -s";
          }

          // CWD.
          //
          if (c.cwd)
          {
            o << " -c ";
            print_path (*c.cwd);
          }

          // Variable unsets/sets.
          //
          auto b (c.variables.begin ()), i (b), e (c.variables.end ());

          // Print a variable name or assignment to the stream, quoting it if
          // necessary.
          //
          auto print = [&o] (const string& v, bool name)
          {
            size_t p (v.find_first_of (" \\\"'"));

            // Print the variable name/assignment as is if it doesn't contain
            // any special characters.
            //
            if (p == string::npos)
            {
              o << v;
              return;
            }

            // If the variable name contains any special characters, then
            // quote the name/assignment as a whole.
            //
            size_t eq;
            if (name || (eq = v.find ('=')) > p)
            {
              to_stream_quoted (o, v);
              return;
            }

            // Finally, if the variable value contains any special characters,
            // then we quote only the value.
            //
            assert (eq != string::npos);

            o.write (v.c_str (), eq + 1);              // Includes '='.
            to_stream_quoted (o, v.c_str () + eq + 1);
          };

          // Variable unsets.
          //
          // Print the variable unsets as the -u options until a variable set
          // is encountered (contains '=') or the end of the variable list is
          // reached.
          //
          // Note that we rely on the fact that unsets come first, which is
          // guaranteed by parser::parse_env_builtin().
          //
          for (; i != e; ++i)
          {
            const string& v (*i);

            if (v.find ('=') == string::npos) // Variable unset.
            {
              o << " -u "; print (v, true /* name*/);
            }
            else                              // Variable set.
              break;
          }

          // Variable sets.
          //
          // Note that we don't add the '-' separator since we always use the
          // `-* <value>` option notation and so there can't be any ambiguity
          // with a variable set.
          //
          for (; i != e; ++i)
          {
            o << ' '; print (*i, false /* name */);
          }

          o << " -- ";
        }

        // Program.
        //
        to_stream_q (o, c.program.recall_string ());

        // Arguments.
        //
        for (const string& a: c.arguments)
        {
          o << ' ';
          to_stream_q (o, a);
        }

        // Redirects.
        //
        if (c.in)
          print_redirect (*c.in, 0);

        if (c.out)
          print_redirect (*c.out, 1);

        if (c.err)
          print_redirect (*c.err, 2);

        for (const auto& p: c.cleanups)
        {
          o << " &";

          if (p.type != cleanup_type::always)
            o << (p.type == cleanup_type::maybe ? '?' : '!');

          print_path (p.path);
        }

        if (c.exit)
        {
          switch (c.exit->comparison)
          {
          case exit_comparison::eq: o << " == "; break;
          case exit_comparison::ne: o << " != "; break;
          }

          o << static_cast<uint16_t> (c.exit->code);
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

    // environment_vars
    //
    environment_vars::iterator environment_vars::
    find (const string& var)
    {
      size_t n (var.find ('='));
      if (n == string::npos)
        n = var.size ();

      return find_if (begin (), end (),
                      [&var, n] (const string& v)
                      {
                        return v.compare (0, n, var, 0, n) == 0 &&
                          (v[n] == '=' || v[n] == '\0');
                      });
    }

    void environment_vars::
    add (string var)
    {
      iterator i (find (var));

      if (i != end ())
        *i = move (var);
      else
        push_back (move (var));
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
          token (move (r.token)),
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
    // environment
    //
    void environment::
    clean (script::cleanup c, bool implicit)
    {
      using script::cleanup;

      // Implicit never-cleanup doesn't make sense.
      //
      assert (!implicit || c.type != cleanup_type::never);

      const path& p (c.path);

      if (sandbox_dir.path != nullptr && !p.sub (*sandbox_dir.path))
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

    const environment_vars& environment::
    exported_variables (environment_vars&)
    {
      return exported_vars;
    }

    const environment_vars& environment::
    merge_exported_variables (const environment_vars& vars,
                              environment_vars& storage)
    {
      const environment_vars& own (exported_variables (storage));

      // If both, the own and the specified variable (un)sets are present,
      // then merge them. Otherwise, return the own (un)sets, if present, or
      // the specified (un)sets otherwise.
      //
      if (!own.empty () && !vars.empty ())
      {
        // Copy the own (un)sets into the storage, if they are not there yet.
        //
        if (&storage != &own)
          storage = own;

        for (const string& v: vars)
          storage.add (v);

        return storage;
      }
      else if (!own.empty ())
        return own;
      else
        return vars;
    }

    // Helpers.
    //
    void
    verify_environment_var_name (const string& name,
                                 const char* prefix,
                                 const location& l,
                                 const char* opt)
    {
      if (name.empty ())
      {
        diag_record dr (fail (l));
        dr << prefix << "empty ";

        if (opt == nullptr)
          dr << "variable name";
        else
          dr << "value for option " << opt;
      }

      if (name.find ('=') != string::npos)
      {
        diag_record dr (fail (l));
        dr << prefix << "invalid ";

        if (opt == nullptr)
          dr << "variable name '" << name << "'";
        else
          dr << "value '" << name << "' for option " << opt;

        dr << ": contains '='";
      }
    }

    void
    verify_environment_var_assignment (const string& var,
                                       const char* prefix,
                                       const location& l)
    {
      size_t p (var.find ('='));

      if (p == 0)
        fail (l) << prefix << "empty variable name";

      if (p == string::npos)
        fail (l) << prefix << "expected variable assignment instead of '"
                 << var << "'";
    }
  }
}
