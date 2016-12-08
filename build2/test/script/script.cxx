// file      : build2/test/script/script.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/script>

#include <sstream>
#include <algorithm> // find()

#include <build2/target>
#include <build2/algorithm>

using namespace std;

namespace build2
{
  namespace test
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

        auto print_redirect =
          [&o, print_path] (const redirect& r, const char* prefix)
        {
          o << ' ' << prefix;

          size_t n (string::traits_type::length (prefix));
          assert (n > 0);

          char d (prefix[n - 1]); // Redirect direction.

          switch (r.type)
          {
          case redirect_type::none:  assert (false);   break;
          case redirect_type::pass:  o << '+';         break;
          case redirect_type::null:  o << '-';         break;
          case redirect_type::merge: o << '&' << r.fd; break;

          case redirect_type::here_str_literal:
            {
              const string& v (r.str);
              bool nl (!v.empty () && v.back () == '\n');

              if (!nl)
                o << ':';

              to_stream_q (o, nl ? string (v, 0, v.size () - 1) : v);
              break;
            }
          case redirect_type::here_doc_literal:
            {
              const string& v (r.str);
              bool nl (!v.empty () && v.back () == '\n');

              // Add another '>' or '<'. Note that here end marker never
              // needs to be quoted.
              //
              o << d << (nl ? "" : ":");
              to_stream_q (o, r.end);
              break;
            }
          case redirect_type::file:
            {
              // Add '>>' or '<<' (and so make it '<<<' or '>>>').
              //
              o << d << d << (r.file.append ? "&" : "");
              print_path (r.file.path);
              break;
            }
          case redirect_type::here_str_regex: // @@ REGEX
          case redirect_type::here_doc_regex: assert (false); break;
          }
        };

        auto print_doc = [&o] (const redirect& r)
        {
          const string& v (r.str);
          bool nl (!v.empty () && v.back () == '\n');
          o << endl << v << (nl ? "" : "\n") << r.end;
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
          if (c.in.type  != redirect_type::none) print_redirect (c.in,   "<");
          if (c.out.type != redirect_type::none) print_redirect (c.out,  ">");
          if (c.err.type != redirect_type::none) print_redirect (c.err, "2>");

          for (const auto& p: c.cleanups)
          {
            o << " &";

            if (p.type != cleanup_type::always)
              o << (p.type == cleanup_type::maybe ? '?' : '!');

            print_path (p.path);
          }

          if (c.exit.comparison != exit_comparison::eq || c.exit.status != 0)
          {
            switch (c.exit.comparison)
            {
            case exit_comparison::eq: o << " == "; break;
            case exit_comparison::ne: o << " != "; break;
            }

            o << static_cast<uint16_t> (c.exit.status);
          }
        }

        if ((m & command_to_stream::here_doc) == command_to_stream::here_doc)
        {
          // Here-documents.
          //
          if (c.in.type  == redirect_type::here_doc_literal) print_doc (c.in);
          if (c.out.type == redirect_type::here_doc_literal) print_doc (c.out);
          if (c.err.type == redirect_type::here_doc_literal) print_doc (c.err);
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
        case redirect_type::merge: break;

        case redirect_type::here_str_literal:
        case redirect_type::here_doc_literal: new (&str) string (); break;

        case redirect_type::here_str_regex:
        case redirect_type::here_doc_regex: new (&regex) regex_type (); break;

        case redirect_type::file: new (&file) file_type (); break;
        }
      }

      redirect::
      redirect (redirect&& r)
          : type (r.type), end (move (r.end))
      {
        switch (type)
        {
        case redirect_type::none:
        case redirect_type::pass:
        case redirect_type::null: break;

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
            new (&regex) regex_type (move (r.regex));
            break;
          }
        case redirect_type::file:
          {
            new (&file) file_type (move (r.file));
            break;
          }
        }
      }

      redirect::
      ~redirect ()
      {
        switch (type)
        {
        case redirect_type::none:
        case redirect_type::pass:
        case redirect_type::null:
        case redirect_type::merge: break;

        case redirect_type::here_str_literal:
        case redirect_type::here_doc_literal: str.~string (); break;

        case redirect_type::here_str_regex:
        case redirect_type::here_doc_regex: regex.~regex_type (); break;

        case redirect_type::file: file.~file_type (); break;
        }
      }

      redirect& redirect::
      operator= (redirect&& r)
      {
        if (this != &r)
        {
          this->~redirect ();
          new (this) redirect (move (r)); // Assume noexcept move-constructor.
        }
        return *this;
      }

      // scope
      //
      scope::
      scope (const string& id, scope* p)
          : parent (p),
            root (p != nullptr ? p->root : static_cast<script*> (this)),
            id_path (cast<path> (assign (root->id_var) = path ())),
            wd_path (cast<dir_path> (assign (root->wd_var) = dir_path ()))

      {
        // Construct the id_path as a string to ensure POSIX form. In fact,
        // the only reason we keep it as a path is to be able to easily get id
        // by calling leaf().
        //
        {
          string s (p != nullptr ? p->id_path.string () : string ());

          if (!s.empty () && !id.empty ())
            s += '/';

          s += id;
          const_cast<path&> (id_path) = path (move (s));
        }

        // Calculate the working directory path unless this is the root scope
        // (handled in an ad hoc way).
        //
        if (p != nullptr)
          const_cast<dir_path&> (wd_path) = dir_path (p->wd_path) /= id;
      }

      void scope::
      clean (cleanup c, bool implicit)
      {
        using std::find; // Hidden by scope::find().

        assert (!implicit || c.type == cleanup_type::always);

        const path& p (c.path);
        if (!p.sub (root->wd_path))
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

      // script_base
      //
      script_base::
      script_base ()
          : // Enter the test.* variables with the same variable types as in
            // buildfiles except for test: while in buildfiles it can be a
            // target name, in testscripts it should be resolved to a path.
            //
            test_var      (var_pool.insert<path> ("test")),
            options_var   (var_pool.insert<strings> ("test.options")),
            arguments_var (var_pool.insert<strings> ("test.arguments")),
            redirects_var (var_pool.insert<strings> ("test.redirects")),
            cleanups_var  (var_pool.insert<strings> ("test.cleanups")),

            wd_var (var_pool.insert<dir_path> ("~")),
            id_var (var_pool.insert<path> ("@")),
            cmd_var (var_pool.insert<strings> ("*")),
            cmdN_var {
              &var_pool.insert<path> ("0"),
              &var_pool.insert<string> ("1"),
              &var_pool.insert<string> ("2"),
              &var_pool.insert<string> ("3"),
              &var_pool.insert<string> ("4"),
              &var_pool.insert<string> ("5"),
              &var_pool.insert<string> ("6"),
              &var_pool.insert<string> ("7"),
              &var_pool.insert<string> ("8"),
              &var_pool.insert<string> ("9")} {}

      // script
      //
      static inline string
      script_id (const path& p)
      {
        string r (p.leaf ().string ());

        if (r == "testscript")
          return string ();

        size_t n (path::traits::find_extension (r));
        assert (n != string::npos);
        r.resize (n);
        return r;
      }

      script::
      script (target& tt, testscript& st, const dir_path& rwd)
          : group (script_id (st.path ())),
            test_target (tt), script_target (st)
      {
        // Set the script working dir ($~) to $out_base/test/<id> (id_path
        // for root is just the id which is empty if st is 'testscript').
        //
        const_cast<dir_path&> (wd_path) = dir_path (rwd) /= id_path.string ();

        // Set the test variable at the script level. We do it even if it's
        // set in the buildfile since they use different types.
        //
        {
          value& v (assign (test_var));

          // Note that the test variable's visibility is target.
          //
          lookup l (find_in_buildfile ("test", false));

          target* t (nullptr);
          if (l.defined ())
          {
            const name* n (cast_null<name> (l));

            if (n == nullptr)
              v = nullptr;
            else if (n->empty ())
              v = path ();
            else if (n->simple ())
            {
              // Ignore the special 'true' value.
              //
              if (n->value != "true")
                v = path (n->value);
              else
                t = &tt;
            }
            else if (n->directory ())
              v = path (n->dir);
            else
            {
              // Must be a target name.
              //
              // @@ OUT: what if this is a @-qualified pair or names?
              //
              t = &search (*n, tt.base_scope ());
            }
          }
          else
            // By default we set it to the test target's path.
            //
            t = &tt;

          // If this is a path-based target, then we use the path. If this
          // is an alias target (e.g., dir{}), then we use the directory
          // path. Otherwise, we leave it NULL expecting the testscript to
          // set it to something appropriate, if used.
          //
          if (t != nullptr)
          {
            if (auto* p = t->is_a<path_target> ())
            {
              // Do some sanity checks: the target better be up-to-date with
              // an assigned path.
              //
              if (p->path ().empty ())
                fail << "target " << *p << " specified in the test variable "
                     << "is out of date" <<
                  info << "consider specifying it as a prerequisite of " << tt;

              v = p->path ();
            }
            else if (t->is_a<alias> ())
              v = path (t->dir);
            else if (t != &tt)
              fail << "target " << *t << " specified in the test variable "
                   << "is not path-based";
          }
        }

        // Set the special $*, $N variables.
        //
        reset_special ();
      }

      lookup scope::
      find (const variable& var) const
      {
        // Search script scopes until we hit the root.
        //
        const scope* p (this);

        do
        {
          if (const value* v = p->vars.find (var))
            return lookup (v, &p->vars);
        }
        while ((p->parent != nullptr ? (p = p->parent) : nullptr) != nullptr);

        return find_in_buildfile (var.name);
      }


      lookup scope::
      find_in_buildfile (const string& n, bool target_only) const
      {
        // Switch to the corresponding buildfile variable. Note that we don't
        // want to insert a new variable into the pool (we might be running
        // in parallel). Plus, if there is no such variable, then we cannot
        // possibly find any value.
        //
        const variable* pvar (build2::var_pool.find (n));

        if (pvar == nullptr)
          return lookup ();

        const script& s (static_cast<const script&> (*root));
        const variable& var (*pvar);

        // First check the target we are testing.
        //
        {
          // Note that we skip applying the override if we did not find any
          // value. In this case, presumably the override also affects the
          // script target and we will pick it up there. A bit fuzzy.
          //
          auto p (s.test_target.find_original (var, target_only));

          if (p.first)
          {
            if (var.override != nullptr)
              p = s.test_target.base_scope ().find_override (
                var, move (p), true);

            return p.first;
          }
        }

        // Then the script target followed by the scopes it is in. Note that
        // while unlikely it is possible the test and script targets will be
        // in different scopes which brings the question of which scopes we
        // should search.
        //
        return s.script_target[var];
      }

      value& scope::
      append (const variable& var)
      {
        lookup l (find (var));

        if (l.defined () && l.belongs (*this)) // Existing var in this scope.
          return const_cast<value&> (*l);

        value& r (assign (var)); // NULL.

        if (l.defined ())
          r = *l; // Copy value (and type) from the outer scope.

        return r;
      }

      void scope::
      reset_special ()
      {
        // First assemble the $* value.
        //
        strings s;

        auto append = [&s] (const strings& v)
        {
          s.insert (s.end (), v.begin (), v.end ());
        };

        if (lookup l = find (root->test_var))
          s.push_back (cast<path> (l).representation ());

        if (lookup l = find (root->options_var))
          append (cast<strings> (l));

        if (lookup l = find (root->arguments_var))
          append (cast<strings> (l));

        // Keep redirects/cleanups out of $N.
        //
        size_t n (s.size ());

        if (lookup l = find (root->redirects_var))
          append (cast<strings> (l));

        if (lookup l = find (root->cleanups_var))
          append (cast<strings> (l));

        // Set the $N values if present.
        //
        for (size_t i (0); i <= 9; ++i)
        {
          value& v (assign (*root->cmdN_var[i]));

          if (i < n)
          {
            if (i == 0)
              v = path (s[i]);
            else
              v = s[i];
          }
          else
            v = nullptr; // Clear any old values.
        }

        // Set $*.
        //
        assign (root->cmd_var) = move (s);
      }
    }
  }
}
