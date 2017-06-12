// file      : build2/version/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
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

#include <build2/version/module.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace version
  {
    // Return true if this prerequisite looks like a project's manifest file.
    // To be sure we would need to search it into target but that we can't
    // do in match().
    //
    static inline bool
    manifest_prerequisite (const scope& rs, const prerequisite_member& p)
    {
      if (!p.is_a<file> () || p.name () != "manifest")
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

    // version_doc
    //
    match_result version_doc::
    match (action a, target& xt, const string&) const
    {
      tracer trace ("version::version_doc::match");

      doc& t (static_cast<doc&> (xt));

      // We match any doc{} target that is called version (potentially with
      // some extension and different case) and that has a dependency on a
      // file called manifest from the same project's src_root.
      //
      if (casecmp (t.name, "version") != 0)
      {
        l4 ([&]{trace << "name mismatch for target " << t;});
        return false;
      }

      const scope& rs (t.root_scope ());

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (manifest_prerequisite (rs, p))
          return true;
      }

      l4 ([&]{trace << "no manifest prerequisite for target " << t;});
      return false;
    }

    recipe version_doc::
    apply (action a, target& xt) const
    {
      doc& t (static_cast<doc&> (xt));

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
      case perform_clean_id:  return &perform_clean; // Standard clean.
      default:                return noop_recipe;    // Configure update.
      }
    }

    target_state version_doc::
    perform_update (action a, const target& xt)
    {
      const doc& t (xt.as<const doc&> ());
      const path& tp (t.path ());

      const scope& rs (t.root_scope ());
      const module& m (*rs.modules.lookup<module> (module::name));

      // Determine if anything needs to be updated.
      //
      // While we use the manifest file to decide whether we need to
      // regenerate the version file, the version itself we get from the
      // module (we checked above that manifest and version files are in the
      // same project).
      //
      // We also have to compare the contents (essentially using the file as
      // its own depdb) in case of a snapshot since we can go back and forth
      // between committed and uncommitted state that doesn't depend on any of
      // our prerequisites.
      //
      {
        optional<target_state> ts (
          execute_prerequisites (a, t, t.load_mtime ()));

        if (ts)
        {
          if (!m.version.snapshot ()) // Everything came from the manifest.
            return *ts;

          try
          {
            ifdstream ifs (tp, fdopen_mode::in, ifdstream::badbit);

            string s;
            getline (ifs, s);

            if (s == m.version.string_project ())
              return *ts;
          }
          catch (const io_error& e)
          {
            fail << "unable to read " << tp << ": " << e;
          }
        }
      }

      if (verb >= 2)
        text << "cat >" << tp;

      try
      {
        ofdstream ofs (tp);
        auto_rmfile arm (tp);
        ofs << m.version.string_project () << endl;
        ofs.close ();
        arm.cancel ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write " << tp << ": " << e;
      }

      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    // version_in
    //
    match_result version_in::
    match (action a, target& xt, const string&) const
    {
      tracer trace ("version::version_in::match");

      file& t (static_cast<file&> (xt));
      const scope& rs (t.root_scope ());

      bool fm (false); // Found manifest.
      bool fi (false); // Found in.
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        fm = fm || manifest_prerequisite (rs, p);
        fi = fi || p.is_a<in> ();
      }

      if (!fm)
        l4 ([&]{trace << "no manifest prerequisite for target " << t;});

      if (!fi)
        l4 ([&]{trace << "no in file prerequisite for target " << t;});

      return fm && fi;
    }

    recipe version_in::
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

    target_state version_in::
    perform_update (action a, const target& xt)
    {
      tracer trace ("version::version_in::perform_update");

      const file& t (xt.as<const file&> ());
      const path& tp (t.path ());

      const scope& rs (t.root_scope ());
      const module& m (*rs.modules.lookup<module> (module::name));

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
        if (dd.expect ("version.in 1") != nullptr)
          l4 ([&]{trace << "rule mismatch forcing update of " << t;});

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

        dd.close ();
      }

      // If nothing changed, then we are done.
      //
      if (!update)
        return ts;

      const string& proj (cast<string> (rs.vars[var_project]));

      // Perform substitutions for the project itself (normally the version.*
      // variables but we allow anything set on the root scope).
      //
      auto subst_self = [&rs] (const location& l, const string& s)
      {
        if (lookup x = rs.vars[s])
        {
          // Call the string() function to convert the value.
          //
          value v (*x);

          return convert<string> (
            functions.call (
              "string", vector_view<value> (&v, 1), l));
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
        text << "ver -o " << tp << ' ' << ip;
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
          // form. Treat $$ as an escape sequence.
          //
          for (size_t b (0), n, d; b != (n = s.size ()); b += d)
          {
            d = 1;

            if (s[b] != '$')
              continue;

            // Find the other end.
            //
            size_t e (b + 1);
            for (; e != (n = s.size ()); ++e)
            {
              if (s[e] == '$')
              {
                if (e + 1 != n && s[e + 1] == '$') // Escape.
                  s.erase (e, 1); // Keep one, erase the other.
                else
                  break;
              }
            }

            if (e == n)
              fail (l) << "unterminated '$'";

            if (e - b == 1) // Escape.
            {
              s.erase (b, 1); // Keep one, erase the other.
              continue;
            }

            // We have a substition with b pointing to the opening $ and e --
            // to the closing. Split it into the package name and the trailer.
            //
            size_t p (s.find ('.', b + 1));

            if (p == string::npos || p > e)
              fail (l) << "invalid substitution: missing package name";

            string sn (s, b + 1, p - b - 1);
            string st (s, p + 1, e - p - 1);
            string sr (sn == proj
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
        fail << "unable to " << what << ' ' << whom << ": " << e;
      }

      t.mtime (system_clock::now ());
      return target_state::changed;
    }
  }
}
