// file      : build2/version/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/rule.hxx>

#include <build2/scope.hxx>
#include <build2/target.hxx>
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

      // Note that while normally we print these at verbosity level 4, these
      // ones get quite noisy since we try this rule any file target.
      //
      if (!fm)
        l5 ([&]{trace << "no manifest prerequisite for target " << t;});

      if (!fi)
        l5 ([&]{trace << "no in file prerequisite for target " << t;});

      bool r (fm && fi);

      // If we match, lookup and cache the module for the update operation.
      //
      if (r && a == perform_update_id)
        t.data (rs.modules.lookup<module> (module::name));

      return r;
    }

    string in_rule::
    lookup (const location& l,
            action a,
            const target& t,
            const string& n) const
    {
      // Note that this code will be executed during up-to-date check for each
      // substitution so let's try not to do anything overly sub-optimal here.
      //
      const module& m (*t.data<const module*> ());

      // Split it into the package name and the variable/condition name.
      //
      // We used to bail if there is no package component but now we treat it
      // the same as project. This can be useful when trying to reuse existing
      // .in files (e.g., from autoconf, etc).
      //
      size_t p (n.find ('.'));

      if (p == string::npos || n.compare (0, p, m.project) == 0)
      {
        return rule::lookup (l, // Standard lookup.
                             a,
                             t,
                             p == string::npos ? n : string (n, p + 1));
      }

      string pn (n, 0, p);
      string vn (n, p + 1);

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
      // Note also that the last two (condition and check) can only be used in
      // the strict substitution mode since in::rule::substitute() will skip
      // them in the lax mode.

      // For now we re-parse the constraint every time. Firstly because not
      // all of them are necessarily in the standard form and secondly because
      // of the MT-safety.
      //
      standard_version_constraint c;

      try
      {
        auto i (m.dependencies.find (pn));

        if (i == m.dependencies.end ())
          fail (l) << "unknown dependency '" << pn << "'";

        if (i->second.empty ())
          fail (l) << "no version constraint for dependency " << pn;

        c = standard_version_constraint (i->second);
      }
      catch (const invalid_argument& e)
      {
        fail (l) << "invalid version constraint for dependency " << pn
                 << ": " << e;
      }

      // Now substitute.
      //
      size_t i;
      if (vn == "version")
      {
        return c.string (); // Use normalized representation.
      }
      if (vn.compare (0, (i = 6),  "check(")     == 0 ||
          vn.compare (0, (i = 10), "condition(") == 0)
      {
        size_t j (vn.find_first_of (",)", i));

        if (j == string::npos || (vn[j] == ',' && vn.back () != ')'))
          fail (l) << "missing closing ')'";

        string vm (vn, i, j - i); // VER macro.
        string sm (vn[j] == ','   // SNAP macro.
                   ? string (vn, j + 1, vn.size () - j - 2)
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

          // Note that version orders everything among pre-releases (that E
          // being 0/1). So the snapshot comparison is only necessary "inside"
          // the same pre-release.
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

        if (vn[1] == 'o') // condition
          return cond ();

        string r;

        // This is tricky: if the version header hasn't been generated yet,
        // then the check will fail. Maybe a better solution is to disable
        // diagnostics and ignore (some) errors during dependency extraction.
        //
        r += "#ifdef " + vm + "\n";
        r += "#  if !(" + cond () + ")\n";
        r += "#    error incompatible " + pn + " version, ";
        r +=       pn + ' ' + c.string () + " is required\n";
        r += "#  endif\n";
        r += "#endif";

        return r;
      }
      else
        fail (l) << "unknown dependency substitution '" << vn << "'" << endf;
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
