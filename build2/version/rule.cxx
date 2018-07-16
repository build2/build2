// file      : build2/version/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/rule.hxx>

#include <build2/depdb.hxx>
#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/context.hxx>
#include <build2/function.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/in/target.hxx>

#include <build2/version/module.hxx>
#include <build2/version/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace version
  {
    using in::in;

    // Return true if this prerequisite is a project's manifest file. To be
    // sure we would need to search it into target but that we can't do in
    // match().
    //
    static inline bool
    manifest_prerequisite (const scope& rs, const prerequisite_member& p)
    {
      if (!p.is_a<manifest> () || p.name () != "manifest")
        return false;

      const scope& s (p.scope ());

      if (s.root_scope () == nullptr) // Out of project prerequisite.
        return false;

      dir_path d (p.dir ());
      if (d.relative ())
        d = s.src_path () / d;
      d.normalize ();

      return d == rs.src_path ();
    }

    // in_rule
    //
    bool in_rule::
    match (action a, target& xt, const string&) const
    {
      tracer trace ("version::in_rule::match");

      file& t (static_cast<file&> (xt));
      const scope& rs (t.root_scope ());

      bool fm (false); // Found manifest.
      bool fi (false); // Found in.
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
          continue;

        fm = fm || manifest_prerequisite (rs, p);
        fi = fi || p.is_a<in> ();
      }

      if (!fm)
        l4 ([&]{trace << "no manifest prerequisite for target " << t;});

      if (!fi)
        l4 ([&]{trace << "no in file prerequisite for target " << t;});

      return fm && fi;
    }

    recipe in_rule::
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
      match_prerequisite_members (a, t);

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id:  return &perform_clean_depdb; // Standard clean.
      default:                return noop_recipe;          // Configure update.
      }
    }

    target_state in_rule::
    perform_update (action a, const target& xt)
    {
      tracer trace ("version::in_rule::perform_update");

      const file& t (xt.as<const file&> ());
      const path& tp (t.path ());

      const scope& rs (t.root_scope ());
      const module& m (*rs.modules.lookup<module> (module::name));

      // The substitution symbol can be overridden with the in.symbol
      // variable.
      //
      char sym ('$');
      if (const string* s = cast_null<string> (t[m.in_symbol]))
      {
        if (s->size () == 1)
          sym = s->front ();
        else
          fail << "invalid substitution symbol '" << *s << "'";
      }

      // The substitution mode can be overridden with the in.substitution
      // variable.
      //
      bool strict (true);
      if (const string* s = cast_null<string> (t[m.in_substitution]))
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

      // We use depdb to track both the .in file and the potentially patched
      // snapshot.
      //
      {
        depdb dd (tp + ".d");

        // First should come the rule name/version.
        //
        if (dd.expect ("version.in 3") != nullptr)
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

        // Finally the snapshot info.
        //
        if (dd.expect (m.version.string_snapshot ()) != nullptr)
          l4 ([&]{trace << "snapshot mismatch forcing update of " << t;});

        // Update if depdb mismatch.
        //
        if (dd.writing () || dd.mtime () > mt)
          update = true;

        //@@ TODO: what if one of the substituted non-version values is
        //   changes. See how we handle this in the .in module. In fact,
        //   it feels like this should be an extended version of in.

        dd.close ();
      }

      // If nothing changed, then we are done.
      //
      if (!update)
        return ts;

      const string& proj (cast<string> (rs.vars[var_project]));

      // Perform substitutions for the project itself (normally the version.*
      // variables but we allow anything set on the target and up).
      //
      auto subst_self = [&t] (const location& l, const string& s) -> string
      {
        if (lookup x = t[s])
        {
          value v (*x);

          // For typed values call the string() function to convert it.
          //
          try
          {
            return convert<string> (
              v.type == nullptr
              ? move (v)
              : functions.call (
                &t.base_scope (), "string", vector_view<value> (&v, 1), l));
          }
          catch (const invalid_argument& e)
          {
            fail (l) << e <<
              info << "while substituting '" << s << "'" << endf;
          }
        }
        else
          fail (l) << "undefined project variable '" << s << "'" << endf;
      };

      // Perform substitutions for a dependency. Here we recognize the
      // following substitutions:
      //
      // $libfoo.version$               - textual version constraint.
      // $libfoo.condition(VER[,SNAP])$ - numeric satisfaction condition.
      // $libfoo.check(VER[,SNAP])$     - numeric satisfaction check (#if ...).
      //
      // Where VER is the version number macro and SNAP is the optional
      // snapshot number macro (only needed if you plan to include snapshot
      // informaton in your constraints).
      //
      auto subst_dep = [&m] (const location& l,
                             const string& n,
                             const string& s)
      {
        // For now we re-parse the constraint every time. Firstly because
        // not all of them are necessarily in the standard form and secondly
        // because of the MT-safety.
        //
        standard_version_constraint c;

        try
        {
          auto i (m.dependencies.find (n));

          if (i == m.dependencies.end ())
            fail (l) << "unknown dependency '" << n << "'";

          if (i->second.empty ())
            fail (l) << "no version constraint for dependency " << n;

          c = standard_version_constraint (i->second);
        }
        catch (const invalid_argument& e)
        {
          fail (l) << "invalid version constraint for dependency " << n
                   << ": " << e;
        }

        // Now substitute.
        //
        size_t i;
        if (s == "version")
        {
          return c.string (); // Use normalized representation.
        }
        if (s.compare (0, (i = 6),  "check(")     == 0 ||
            s.compare (0, (i = 10), "condition(") == 0)
        {
          size_t j (s.find_first_of (",)", i));

          if (j == string::npos || (s[j] == ',' && s.back () != ')'))
            fail (l) << "missing closing ')'";

          string vm (s, i, j - i); // VER macro.
          string sm (s[j] == ','   // SNAP macro.
                     ? string (s, j + 1, s.size () - j - 2)
                     : string ());

          trim (vm);
          trim (sm);

          auto cond = [&l, &c, &vm, &sm] () -> string
          {
            auto& miv (c.min_version);
            auto& mav (c.max_version);

            bool mio (c.min_open);
            bool mao (c.max_open);

            if (sm.empty () &&
                ((miv && miv->snapshot ()) ||
                 (mav && mav->snapshot ())))
              fail (l) << "snapshot macro required for " << c.string ();

            auto cmp = [] (const string& m, const char* o, uint64_t v)
            {
              return m + o + to_string (v) + "ULL";
            };

            // Note that version orders everything among pre-releases (that
            // E being 0/1). So the snapshot comparison is only necessary
            // "inside" the same pre-release.
            //
            auto max_cmp = [&vm, &sm, mao, &mav, &cmp] (bool p = false)
            {
              string r;

              if (mav->snapshot ())
              {
                r += (p ? "(" : "");

                r += cmp (vm, " < ", mav->version) + " || (";
                r += cmp (vm, " == ", mav->version) + " && ";
                r += cmp (sm, (mao ? " < " : " <= "), mav->snapshot_sn) + ")";

                r += (p ? ")" : "");
              }
              else
                r = cmp (vm, (mao ? " < " : " <= "), mav->version);

              return r;
            };

            auto min_cmp = [&vm, &sm, mio, &miv, &cmp] (bool p = false)
            {
              string r;

              if (miv->snapshot ())
              {
                r += (p ? "(" : "");

                r += cmp (vm, " > ", miv->version) + " || (";
                r += cmp (vm, " == ", miv->version) + " && ";
                r += cmp (sm, (mio ? " > " : " >= "), miv->snapshot_sn) + ")";

                r += (p ? ")" : "");
              }
              else
                r = cmp (vm, (mio ? " > " : " >= "), miv->version);

              return r;
            };

            // < / <=
            //
            if (!miv)
              return max_cmp ();

            // > / >=
            //
            if (!mav)
              return min_cmp ();

            // ==
            //
            if (*miv == *mav)
            {
              string r (cmp (vm, " == ", miv->version));

              if (miv->snapshot ())
                r += " && " + cmp (sm, " == ", miv->snapshot_sn);

              return r;
            }

            // range
            //
            return min_cmp (true) + " && " + max_cmp (true);
          };

          if (s[1] == 'o') // condition
            return cond ();

          string r;

          // This is tricky: if the version header hasn't been generated yet,
          // then the check will fail. Maybe a better solution is to disable
          // diagnostics and ignore (some) errors during dependency
          // extraction.
          //
          r += "#ifdef " + vm + "\n";
          r += "#  if !(" + cond () + ")\n";
          r += "#    error incompatible " + n + " version, ";
          r +=       n + ' ' + c.string () + " is required\n";
          r += "#  endif\n";
          r += "#endif";

          return r;
        }
        else
          fail (l) << "unknown dependency substitution '" << s << "'" << endf;
      };

      if (verb >= 2)
        text << "ver " << ip << " >" << tp;
      else if (verb)
        text << "ver " << ip;

      // Read and process the file, one line at a time.
      //
      const char* what;
      const path* whom;
      try
      {
        what = "open"; whom = &ip;
        ifdstream ifs (ip, fdopen_mode::in, ifdstream::badbit);

        what = "open"; whom = &tp;
        ofdstream ofs (tp);
        auto_rmfile arm (tp);

        string s; // Reuse the buffer.
        for (size_t ln (1);; ++ln)
        {
          what = "read"; whom = &ip;
          if (!getline (ifs, s))
            break; // Could not read anything, not even newline.

          const location l (&ip, ln); // Not tracking column for now.

          // Scan the line looking for substiutions in the $<pkg>.<rest>$
          // form. In the strict mode treat $$ as an escape sequence.
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

            // We have a (potential in the lax mode) substition with b
            // pointing to the opening symbol and e -- to the closing.
            //
            if (!strict)
            {
              // Scan the fragment to make sure it is a variable name (that
              // is, it can be expanded as just $<name>; see lexer's variable
              // mode for details).
              //
              size_t i;
              for (i = b + 1; i != e; )
              {
                bool f (i == b + 1); // First.
                char c (s[i++]);
                bool l (i == e);     // Last.

                if (c == '_' || (f ? alpha (c) : alnum (c)))
                  continue;

                if (c == '.' && !l)
                  continue;

                i = string::npos;
                break;
              }

              if (i == string::npos)
              {
                d = e - b + 1; // Ignore this substitution.
                continue;
              }
            }

            // Split it into the package name and the trailer.
            //
            // We used to bail if there is no package component but now we
            // treat it the same as project. This can be useful when trying to
            // reuse existing .in files (e.g., from autoconf, etc).
            //
            string sn, st;
            size_t p (s.find ('.', b + 1));

            if (p != string::npos && p < e)
            {
              sn.assign (s, b + 1, p - b - 1);
              st.assign (s, p + 1, e - p - 1);
            }
            else
              st.assign (s, b + 1, e - b - 1);

            string sr (sn.empty () || sn == proj
                       ? subst_self (l, st)
                       : subst_dep (l, sn, st));

            // Patch the result in and adjust the delta.
            //
            s.replace (b, e - b + 1, sr);
            d = sr.size ();
          }

          what = "write"; whom = &tp;
          ofs << s << endl;
        }

        what = "close"; whom = &tp;
        ofs.close ();
        arm.cancel ();

        what = "close"; whom = &ip;
        ifs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to " << what << ' ' << *whom << ": " << e;
      }

      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    // manifest_install_rule
    //
    bool manifest_install_rule::
    match (action a, target& t, const string&) const
    {
      // We only match project's manifest.
      //
      if (!t.is_a<manifest> () || t.name != "manifest")
        return false;

      // Must be in project's src_root.
      //
      const scope& s (t.base_scope ());
      if (s.root_scope () != &s || s.src_path () != t.dir)
        return false;

      return file_rule::match (a, t, "");
    }

    auto_rmfile manifest_install_rule::
    install_pre (const file& t, const install_dir&) const
    {
      const path& p (t.path ());

      const scope& rs (t.root_scope ());
      const module& m (*rs.modules.lookup<module> (module::name));

      if (!m.rewritten)
        return auto_rmfile (p, false /* active */);

      // Our options are to use path::temp_path() or to create a .t file in
      // the out tree. Somehow the latter feels more appropriate (even though
      // if we crash in between, we won't clean it up).
      //
      return fixup_manifest (p, rs.out_path () / "manifest.t", m.version);
    }
  }
}
