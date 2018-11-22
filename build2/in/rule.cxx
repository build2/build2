// file      : build2/in/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/in/rule.hxx>

#include <cstdlib> // strtoull()

#include <build2/depdb.hxx>
#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/function.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/in/target.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace in
  {
    bool rule::
    match (action a, target& xt, const string&) const
    {
      tracer trace ("in::rule::match");

      if (!xt.is_a<file> ()) // See module init() for details.
        return false;

      file& t (static_cast<file&> (xt));

      bool fi (false); // Found in.
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
          continue;

        fi = fi || p.is_a<in> ();
      }

      // Note that while normally we print these at verbosity level 4, this
      // one gets quite noisy since we try this rule for any file target.
      //
      if (!fi)
        l5 ([&]{trace << "no in file prerequisite for target " << t;});

      return fi;
    }

    recipe rule::
    apply (action a, target& xt) const
    {
      file& t (static_cast<file&> (xt));

      // Derive the file name.
      //
      t.derive_path ();

      // Inject dependency on the output directory.
      //
      inject_fsdir (a, t);

      // Match prerequisite members.
      //
      match_prerequisite_members (a,
                                  t,
                                  [this] (action a,
                                          const target& t,
                                          const prerequisite_member& p,
                                          include_type i)
                                  {
                                    return search (a, t, p, i);
                                  });

      switch (a)
      {
      case perform_update_id: return [this] (action a, const target& t)
        {
          return perform_update (a, t);
        };
      case perform_clean_id: return &perform_clean_depdb; // Standard clean.
      default:               return noop_recipe;          // Configure update.
      }
    }

    target_state rule::
    perform_update (action a, const target& xt) const
    {
      tracer trace ("in::rule::perform_update");

      const file& t (xt.as<const file&> ());
      const path& tp (t.path ());

      // Substitution symbol.
      //
      char sym (symbol_);
      if (const string* s = cast_null<string> (t["in.symbol"]))
      {
        if (s->size () == 1)
          sym = s->front ();
        else
          fail << "invalid substitution symbol '" << *s << "'";
      }

      // Substitution mode.
      //
      bool strict (strict_);
      if (const string* s = cast_null<string> (t["in.substitution"]))
      {
        if (*s == "lax")
          strict = false;
        else if (*s != "strict")
          fail << "invalid substitution mode '" << *s << "'";
      }

      // Determine if anything needs to be updated.
      //
      timestamp mt (t.load_mtime ());
      auto pr (execute_prerequisites<in> (a, t, mt));

      bool update (!pr.first);
      target_state ts (update ? target_state::changed : *pr.first);

      const in& i (pr.second);
      const path& ip (i.path ());

      // We use depdb to track changes to the .in file name, symbol/mode, and
      // variable values that have been substituted.
      //
      depdb dd (tp + ".d");

      // First should come the rule name/version.
      //
      if (dd.expect (rule_id_ + " 1") != nullptr)
        l4 ([&]{trace << "rule mismatch forcing update of " << t;});

      // Then the substitution symbol.
      //
      if (dd.expect (string (1, sym)) != nullptr)
        l4 ([&]{trace << "substitution symbol mismatch forcing update of"
                      << t;});

      // Then the substitution mode.
      //
      if (dd.expect (strict ? "strict" : "lax") != nullptr)
        l4 ([&]{trace << "substitution mode mismatch forcing update of"
                      << t;});

      // Then the .in file.
      //
      if (dd.expect (i.path ()) != nullptr)
        l4 ([&]{trace << "in file mismatch forcing update of " << t;});

      // Update if any mismatch or depdb is newer that the output.
      //
      if (dd.writing () || dd.mtime > mt)
        update = true;

      // Substituted variable values.
      //
      // The plan is to save each substituted variable name and the hash of
      // its value one entry per line. Plus the line location of its expansion
      // for diagnostics.
      //
      // If update is true (i.e., the .in file has changes), then we simply
      // overwrite the whole list.
      //
      // If update is false, then we need to read each name/hash, query and
      // hash its current value, and compare. If hashes differ, then we need
      // to start overwriting from this variable (the prefix of variables
      // couldn't have changed since the .in file hasn't changed).
      //
      // Note that if the .in file substitutes the same variable multiple
      // times, then we will end up with multiple entries for such a variable.
      // For now we assume this is ok since this is probably not very common
      // and it makes the overall logic simpler.
      //
      size_t dd_skip (0); // Number of "good" variable lines.

      if (update)
      {
        // If we are still reading, mark the next line for overwriting.
        //
        if (dd.reading ())
        {
          dd.read ();  // Read the first variable line, if any.
          dd.write (); // Mark it for overwriting.
        }
      }
      else
      {
        while (dd.more ())
        {
          if (string* s = dd.read ())
          {
            // The line format is:
            //
            // <ln> <name> <hash>
            //
            // Note that <name> can contain spaces (see the constraint check
            // expressions in the version module).
            //
            char* e (nullptr);
            uint64_t ln (strtoull (s->c_str (), &e, 10));

            size_t p1 (*e == ' ' ? e - s->c_str () : string::npos);
            size_t p2 (s->rfind (' '));

            if (p1 != string::npos && p2 != string::npos && p2 - p1 > 1)
            {
              string n (*s, p1 + 1, p2 - p1 - 1);

              // Note that we have to call substitute(), not lookup() since it
              // can be overriden with custom substitution semantics.
              //
              optional<string> v (
                substitute (location (&ip, ln), a, t, n, strict));

              assert (v); // Rule semantics change without version increment?

              if (s->compare (p2 + 1,
                              string::npos,
                              sha256 (*v).string ()) == 0)
              {
                dd_skip++;
                continue;
              }
              else
                l4 ([&]{trace << n << " variable value mismatch forcing "
                              << "update of " << t;});
              // Fall through.
            }

            dd.write (); // Mark this line for overwriting.

            // Fall through.
          }

          break;
        }
      }

      if (dd.writing ()) // Recheck.
        update = true;

      // If nothing changed, then we are done.
      //
      if (!update)
      {
        dd.close ();
        return ts;
      }

      if (verb >= 2)
        text << program_ << ' ' << ip << " >" << tp;
      else if (verb)
        text << program_ << ' ' << ip;

      // Read and process the file, one line at a time.
      //
      const char* what;
      const path* whom;
      try
      {
        what = "open"; whom = &ip;
        ifdstream ifs (ip, fdopen_mode::in, ifdstream::badbit);

        // See fdopen() for details (umask, etc).
        //
        permissions prm (permissions::ru | permissions::wu |
                         permissions::rg | permissions::wg |
                         permissions::ro | permissions::wo);

        if (t.is_a<exe> ())
          prm |= permissions::xu | permissions::xg | permissions::xo;

        // Remove the existing file to make sure permissions take effect.
        //
        rmfile (tp, 3 /* verbosity */);

        what = "open"; whom = &tp;
        ofdstream ofs (fdopen (tp,
                               fdopen_mode::out | fdopen_mode::create,
                               prm));
        auto_rmfile arm (tp);

        string s; // Reuse the buffer.
        for (size_t ln (1);; ++ln)
        {
          what = "read"; whom = &ip;
          if (!getline (ifs, s))
            break; // Could not read anything, not even newline.

          // Not tracking column for now (see also depdb above).
          //
          const location l (&ip, ln);

          // Scan the line looking for substiutions in the $<name>$ form. In
          // the strict mode treat $$ as an escape sequence.
          //
          for (size_t b (0), n, d; b != (n = s.size ()); b += d)
          {
            d = 1;

            if (s[b] != sym)
              continue;

            // Note that in the lax mode these should still be substitutions:
            //
            // @project@@
            // @@project@

            // Find the other end.
            //
            size_t e (b + 1);
            for (; e != (n = s.size ()); ++e)
            {
              if (s[e] == sym)
              {
                if (strict && e + 1 != n && s[e + 1] == sym) // Escape.
                  s.erase (e, 1); // Keep one, erase the other.
                else
                  break;
              }
            }

            if (e == n)
            {
              if (strict)
                fail (l) << "unterminated '" << sym << "'" << endf;

              break;
            }

            if (e - b == 1) // Escape (or just double symbol in the lax mode).
            {
              if (strict)
                s.erase (b, 1); // Keep one, erase the other.

              continue;
            }

            // We have a (potential, in the lax mode) substition with b
            // pointing to the opening symbol and e -- to the closing.
            //
            string name (s, b + 1, e - b -1);
            if (optional<string> val = substitute (l, a, t, name, strict))
            {
              // Save in depdb.
              //
              if (dd_skip == 0)
              {
                // The line format is:
                //
                // <ln> <name> <hash>
                //
                string s (to_string (ln));
                s += ' ';
                s += name;
                s += ' ';
                s += sha256 (*val).string ();
                dd.write (s);
              }
              else
                --dd_skip;

              // Patch the result in and adjust the delta.
              //
              s.replace (b, e - b + 1, *val);
              d = val->size ();
            }
            else
              d = e - b + 1; // Ignore this substitution.
          }

          what = "write"; whom = &tp;
          if (ln != 1)
            ofs << endl; // See below.
          ofs << s;
        }

        // Close depdb before closing the output file so its mtime is not
        // newer than of the output.
        //
        dd.close ();

        what = "close"; whom = &tp;
        ofs << endl; // Last write to make sure our mtime is older than dd.
        ofs.close ();
        arm.cancel ();

        what = "close"; whom = &ip;
        ifs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to " << what << ' ' << *whom << ": " << e;
      }

      dd.verify (tp);

      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    prerequisite_target rule::
    search (action,
            const target& t,
            const prerequisite_member& p,
            include_type i) const
    {
      return prerequisite_target (&build2::search (t, p), i);
    }

    string rule::
    lookup (const location& l, action, const target& t, const string& n) const
    {
      if (auto x = t[n])
      {
        value v (*x);

        // For typed values call string() for conversion.
        //
        try
        {
          return convert<string> (
            v.type == nullptr
            ? move (v)
            : functions.call (&t.base_scope (),
                              "string",
                              vector_view<value> (&v, 1),
                              l));
        }
        catch (const invalid_argument& e)
        {
          fail (l) << e <<
            info << "while substituting '" << n << "'" << endf;
        }
      }
      else
        fail (l) << "undefined variable '" << n << "'" << endf;
    }

    optional<string> rule::
    substitute (const location& l,
                action a,
                const target& t,
                const string& n,
                bool strict) const
    {
      // In the lax mode scan the fragment to make sure it is a variable name
      // (that is, it can be expanded in a buildfile as just $<name>; see
      // lexer's variable mode for details).
      //
      if (!strict)
      {
        for (size_t i (0), e (n.size ()); i != e; )
        {
          bool f (i == 0); // First.
          char c (n[i++]);
          bool l (i == e); // Last.

          if (c == '_' || (f ? alpha (c) : alnum (c)))
            continue;

          if (c == '.' && !l)
            continue;

          return nullopt; // Ignore this substitution.
        }
      }

      return lookup (l, a, t, n);
    }
  }
}
