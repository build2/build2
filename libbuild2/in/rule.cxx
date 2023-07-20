// file      : libbuild2/in/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/in/rule.hxx>

#include <cstdlib> // strtoull()

#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/in/target.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace in
  {
    bool rule::
    match (action a, target& xt) const
    {
      tracer trace ("in::rule::match");

      if (!xt.is_a<file> ()) // See module init() for details.
        return false;

      file& t (xt.as<file> ());

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

      // If we match, derive the file name here instead of in apply() to make
      // it available early for the in{} prerequisite search (see
      // install::file_rule::apply_impl() for background).
      //
      if (fi)
        t.derive_path ();

      return fi;
    }

    recipe rule::
    apply (action a, target& xt) const
    {
      file& t (xt.as<file> ());

      // Make sure derived rules assign the path in match().
      //
      assert (!t.path ().empty ());

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
      if (const string* s = cast_null<string> (t["in.mode"]))
      {
        if (*s == "lax")
          strict = false;
        else if (*s != "strict")
          fail << "invalid substitution mode '" << *s << "'";
      }

      // Substitution map.
      //
      const substitution_map* smap (
        cast_null<map<string, optional<string>>> (t["in.substitutions"]));

      // NULL substitutions.
      //
      optional<string> null;
      if (const string* s = cast_null<string> (t["in.null"]))
        null = *s;
      else
        null = null_;

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

      // Then additional depdb entries, if any.
      //
      perform_update_depdb (a, t, dd);

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
      // Note also that because updating the depdb essentially requires
      // performing the substitutions, this rule ignored the dry-run mode.
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
            // <ln> <name> <hash>[/<flags>]
            //
            // Note that <name> can contain spaces (see the constraint check
            // expressions in the version module). That's the reason why we
            // use the `/` separator for <flags> instead of the more natural
            // space.
            //
            char* e (nullptr);
            uint64_t ln (strtoull (s->c_str (), &e, 10));

            size_t p1 (*e == ' ' ? e - s->c_str () : string::npos); // <name>
            size_t p2 (s->rfind (' '));                             // <hash>

            if (p1 != string::npos && p2 != string::npos && p2 - p1 > 1)
            {
              ++p1;
              string name (*s, p1, p2 - p1);

              ++p2;
              size_t p3 (s->find ('/', p2)); // <flags>

              optional<uint64_t> flags;
              if (p3 != string::npos)
              {
                uint64_t v (strtoull (s->c_str () + p3 + 1, &e, 10));
                if (*e == '\0')
                  flags = v;
              }

              if (p3 == string::npos || flags)
              {
                // Note that we have to call substitute(), not lookup() since
                // it can be overriden with custom substitution semantics.
                //
                optional<string> v (
                  substitute (location (ip, ln),
                              a, t,
                              name, flags,
                              strict, smap, null));

                assert (v); // Rule semantics change without version increment?

                if (p3 != string::npos)
                  p3 -= p2; // Hash length.

                if (s->compare (p2, p3, sha256 (*v).string ()) == 0)
                {
                  dd_skip++;
                  continue;
                }
                else
                  l4 ([&]{trace << name << " variable value mismatch forcing "
                                << "update of " << t;});
              }

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
      {
        // If we straight print the target, in most cases we will end up with
        // something ugly like in{version...h.in} (due to the in{} target
        // type search semantics). There is the `...h` part but also the
        // `.in` part that is redundant given in{}. So let's tidy this up
        // a bit if the extension could have been derived by in_search().
        //
        target_key ik (i.key ());

        if (ik.ext)
        {
          string& ie (*ik.ext);
          const string* te (t.ext ());

          size_t in (ie.size ());
          size_t tn (te != nullptr ? te->size () : 0);

          if (in == tn + (tn != 0 ? 1 : 0) + 2) // [<te>.]in
          {
            if (ie.compare (in - 2, 2, "in") == 0)
            {
              if (tn == 0 || (ie.compare (0, tn, *te) == 0 && ie[tn] == '.'))
                ie.clear ();
            }
          }
        }

        print_diag (program_.c_str (), move (ik), t);
      }

      // Read and process the file, one line at a time, while updating depdb.
      //
      const char* what;
      const path* whom;
      try
      {
        // Open the streams in the binary mode to preserve the .in file line
        // endings.
        //
        what = "open"; whom = &ip;
        ifdstream ifs (ip, fdopen_mode::binary, ifdstream::badbit);

        what = "open"; whom = &tp;
#ifdef _WIN32
        // We don't need to worry about permissions on Windows and trying to
        // remove the file immediately before creating it sometimes can cause
        // open to fail with permission denied.
        //
        ofdstream ofs (tp, fdopen_mode::binary);
#else
        // See fdopen() for details (umask, etc).
        //
        permissions prm (permissions::ru | permissions::wu |
                         permissions::rg | permissions::wg |
                         permissions::ro | permissions::wo);

        if (t.is_a<exe> ())
          prm |= permissions::xu | permissions::xg | permissions::xo;

        // Remove the existing file to make sure permissions take effect. If
        // this fails then presumable writing to it will fail as well and we
        // will complain there.
        //
        try_rmfile (tp, true /* ignore_error */);

        // Note: no binary flag is added since this is noop on POSIX.
        //
        ofdstream ofs (fdopen (tp,
                               fdopen_mode::out | fdopen_mode::create,
                               prm));
#endif
        auto_rmfile arm (tp);

        // Note: this default will only be used if the file is empty (i.e.,
        // does not contain even a newline).
        //
        const char* nl (
#ifdef _WIN32
          "\r\n"
#else
          "\n"
#endif
        );

        uint64_t ln (1);
        for (string s;; ++ln)
        {
          what = "read"; whom = &ip;
          if (!getline (ifs, s))
            break; // Could not read anything, not even newline.

          // Remember the line ending type and, if it is CRLF, strip the
          // trailing '\r'.
          //
          bool crlf (!s.empty () && s.back() == '\r');
          if (crlf)
            s.pop_back();

          what = "write"; whom = &tp;
          if (ln != 1)
            ofs << nl;

          nl = crlf ? "\r\n" : "\n"; // Preserve the original line ending.

          if (ln == 1)
            perform_update_pre (a, t, ofs, nl);

          // Not tracking column for now (see also depdb above).
          //
          process (location (ip, ln),
                   a, t,
                   dd, dd_skip,
                   s, 0,
                   nl, sym, strict, smap, null);

          ofs << s;
        }

        what = "write"; whom = &tp;
        if (ln == 1)
          perform_update_pre (a, t, ofs, nl);
        perform_update_post (a, t, ofs, nl);

        // Close depdb before closing the output file so its mtime is not
        // newer than of the output.
        //
        dd.close ();

        what = "close"; whom = &tp;
        ofs << nl; // Last write to make sure our mtime is older than dd.
        ofs.close ();
        arm.cancel ();

        what = "close"; whom = &ip;
        ifs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to " << what << ' ' << *whom << ": " << e;
      }

      dd.check_mtime (tp);

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

    void rule::
    perform_update_depdb (action, const target&, depdb&) const
    {
    }

    void rule::
    perform_update_pre (action, const target&, ofdstream&, const char*) const
    {
    }

    void rule::
    perform_update_post (action, const target&, ofdstream&, const char*) const
    {
    }

    void rule::
    process (const location& l,
             action a, const target& t,
             depdb& dd, size_t& dd_skip,
             string& s, size_t b,
             const char* nl,
             char sym,
             bool strict,
             const substitution_map* smap,
             const optional<string>& null) const
    {
      // Scan the line looking for substiutions in the $<name>$ form. In the
      // strict mode treat $$ as an escape sequence.
      //
      for (size_t n, d; b != (n = s.size ()); b += d)
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
            fail (l) << "unterminated '" << sym << "'";

          break;
        }

        if (e - b == 1) // Escape (or just double symbol in the lax mode).
        {
          if (strict)
            s.erase (b, 1); // Keep one, erase the other.

          continue;
        }

        // We have a (potential, in the lax mode) substition with b pointing
        // to the opening symbol and e -- to the closing.
        //
        if (optional<string> val = substitute (l,
                                               a, t,
                                               dd, dd_skip,
                                               string (s, b + 1, e - b -1),
                                               nullopt /* flags */,
                                               strict, smap, null))
        {
          replace_newlines (*val, nl);

          // Patch the result in and adjust the delta.
          //
          s.replace (b, e - b + 1, *val);
          d = val->size ();
        }
        else
          d = e - b + 1; // Ignore this substitution.
      }
    }

    optional<string> rule::
    substitute (const location& l,
                action a, const target& t,
                depdb& dd, size_t& dd_skip,
                const string& n,
                optional<uint64_t> flags,
                bool strict,
                const substitution_map* smap,
                const optional<string>& null) const
    {
      optional<string> val (substitute (l, a, t, n, flags, strict, smap, null));

      if (val)
      {
        // Save in depdb.
        //
        if (dd_skip == 0)
        {
          // The line format is:
          //
          // <ln> <name> <hash>[/<flags>]
          //
          string s (to_string (l.line));
          s += ' ';
          s += n;
          s += ' ';
          s += sha256 (*val).string ();
          if (flags)
          {
            s += '/';
            s += to_string (*flags);
          }
          dd.write (s);
        }
        else
          --dd_skip;
      }

      return val;
    }

    optional<string> rule::
    substitute (const location& l,
                action a, const target& t,
                const string& n,
                optional<uint64_t> flags,
                bool strict,
                const substitution_map* smap,
                const optional<string>& null) const
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

      return lookup (l, a, t, n, flags, smap, null);
    }

    string rule::
    lookup (const location& loc,
            action, const target& t,
            const string& n,
            optional<uint64_t> flags,
            const substitution_map* smap,
            const optional<string>& null) const
    {
      assert (!flags);

      // First look in the substitution map.
      //
      if (smap != nullptr)
      {
        auto i (smap->find (n));

        if (i != smap->end ())
        {
          if (i->second)
            return *i->second;

          if (null)
            return *null;

          fail (loc) << "null value in substitution map entry '" << n << "'" <<
            info << "use in.null to specify null value substiution string";
        }
      }

      // Next look for the buildfile variable.
      //
      auto l (t[n]);

      if (l.defined ())
      {
        value v (*l);

        if (v.null)
        {
          if (null)
            return *null;

          fail (loc) << "null value in variable '" << n << "'" <<
            info << "use in.null to specify null value substiution string";
        }

        // For typed values call string() for conversion.
        //
        try
        {
          return convert<string> (
            v.type == nullptr
            ? move (v)
            : t.ctx.functions.call (&t.base_scope (),
                                    "string",
                                    vector_view<value> (&v, 1),
                                    loc));
        }
        catch (const invalid_argument& e)
        {
          fail (loc) << e <<
            info << "while substituting '" << n << "'" << endf;
        }
      }
      else
        fail (loc) << "undefined variable '" << n << "'" << endf;
    }
  }
}
