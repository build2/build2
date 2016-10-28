// file      : build2/test/script/script.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/script>

#include <sstream>
#include <algorithm> // find()

#include <build2/target>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      // Utility functions
      //
      // Quote if empty or contains spaces or any of the special characters.
      //
      // @@ What if it contains quotes, escapes?
      //
      static void
      to_stream_q (ostream& o, const string& s)
      {
        if (s.empty () || s.find_first_of (" |&<>=") != string::npos)
          o << '"' << s << '"';
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

          case redirect_type::here_string:
            {
              const string& v (r.str);
              bool nl (!v.empty () && v.back () == '\n');

              if (!nl)
                o << ':';

              to_stream_q (o, nl ? string (v, 0, v.size () - 1) : v);
              break;
            }
          case redirect_type::here_document:
            {
              const string& v (r.doc.doc);
              bool nl (!v.empty () && v.back () == '\n');

              // Add another '>' or '<'. Note that here end marker never
              // needs to be quoted.
              //
              o << d << (nl ? "" : ":") << r.doc.end;
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
          }
        };

        auto print_doc = [&o] (const redirect& r)
        {
          const string& v (r.doc.doc);
          bool nl (!v.empty () && v.back () == '\n');
          o << endl << v << (nl ? "" : "\n") << r.doc.end;
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
            print_path (p);
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
          if (c.in.type  == redirect_type::here_document) print_doc (c.in);
          if (c.out.type == redirect_type::here_document) print_doc (c.out);
          if (c.err.type == redirect_type::here_document) print_doc (c.err);
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

        case redirect_type::here_string:   new (&str)  string ();    break;
        case redirect_type::here_document: new (&doc)  doc_type ();  break;
        case redirect_type::file:          new (&file) file_type (); break;
        }
      }

      redirect::
      redirect (redirect&& r)
          : type (r.type)
      {
        switch (type)
        {
        case redirect_type::none:
        case redirect_type::pass:
        case redirect_type::null: break;

        case redirect_type::merge: fd = r.fd; break;

        case redirect_type::here_string:
          {
            new (&str) string (move (r.str));
            break;
          }
        case redirect_type::here_document:
          {
            new (&doc) doc_type (move (r.doc));
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
      redirect (const redirect& r)
          : type (r.type)
      {
        switch (type)
        {
        case redirect_type::none:
        case redirect_type::pass:
        case redirect_type::null: break;

        case redirect_type::merge: fd = r.fd; break;

        case redirect_type::here_string:   new (&str) string (r.str);   break;
        case redirect_type::here_document: new (&doc) doc_type (r.doc); break;
        case redirect_type::file:
          {
            new (&file) file_type (r.file);
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

        case redirect_type::here_string:   str.~string ();     break;
        case redirect_type::here_document: doc.~doc_type ();   break;
        case redirect_type::file:          file.~file_type (); break;
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

      redirect& redirect::
      operator= (const redirect& r)
      {
        if (this != &r)
          *this = redirect (r); // Reduce to move-assignment.
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

      // command
      //
      void scope::
      clean (path p)
      {
        using std::find; // Hidden by scope::find().

        if (find (cleanups.begin (), cleanups.end (), p) == cleanups.end ())
          cleanups.emplace_back (move (p));
      }

      // script_base
      //
      script_base::
      script_base ()
          : // Enter the test* variables with the same variable types as in
            // buildfiles.
            //
            test_var (var_pool.insert<path> ("test")),
            opts_var (var_pool.insert<strings> ("test.options")),
            args_var (var_pool.insert<strings> ("test.arguments")),

            cmd_var (var_pool.insert<strings> ("*")),
            wd_var (var_pool.insert<dir_path> ("~")),
            id_var (var_pool.insert<path> ("@")) {}

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

        // Unless we have the test variable set on the test or script target,
        // set it at the script level to the test target's path.
        //
        if (!find (test_var))
        {
          value& v (assign (test_var));

          // If this is a path-based target, then we use the path. If this is
          // an alias target (e.g., dir{}), then we use the directory path.
          // Otherwise, we leave it NULL expecting the testscript to set it to
          // something appropriate, if used.
          //
          if (auto* p = tt.is_a<path_target> ())
            v = p->path ();
          else if (tt.is_a<alias> ())
            v = path (tt.dir.string ()); // Strip trailing slash.
        }

        // Also add the NULL $* value that signals it needs to be recalculated
        // on first access.
        //
        assign (cmd_var) = nullptr;
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
        while (p->parent != nullptr ? (p = p->parent) : nullptr);

        // Switch to the corresponding buildfile variable. Note that we don't
        // want to insert a new variable into the pool (we might be running
        // concurrently). Plus, if there is no such variable, then we cannot
        // possibly find any value.
        //
        const variable* pvar (build2::var_pool.find (var.name));

        if (pvar == nullptr)
          return lookup ();

        const script& s (static_cast<const script&> (*p));
        {
          const variable& var (*pvar);

          // First check the target we are testing.
          //
          {
            // Note that we skip applying the override if we did not find any
            // value. In this case, presumably the override also affects the
            // script target and we will pick it up there. A bit fuzzy.
            //
            auto p (s.test_target.find_original (var, true));

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
      }

      value& scope::
      append (const variable& var)
      {
        lookup l (find (var));

        if (l.defined () && l.belongs (*this)) // Existing var in this scope.
          return const_cast<value&> (*l);

        value& r (assign (var)); // NULL.

        //@@ I guess this is where we convert untyped value to strings?
        //
        if (l.defined ())
          r = *l; // Copy value (and type) from the outer scope.

        return r;
      }
    }
  }
}
