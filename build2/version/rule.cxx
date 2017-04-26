// file      : build2/version/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/rule>

#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/algorithm>
#include <build2/filesystem>
#include <build2/diagnostics>

#include <build2/version/module>

using namespace std;
using namespace butl;

namespace build2
{
  namespace version
  {
    match_result version_doc::
    match (action a, target& xt, const string&) const
    {
      tracer trace ("version::version_file::match");

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
        if (!p.is_a<file> ())
          continue;

        const target& pt (p.search ());

        if (pt.name != "manifest")
          continue;

        const scope* prs (pt.base_scope ().root_scope ());

        if (prs == nullptr || prs != &rs || pt.dir != rs.src_path ())
          continue;

        return true;
      }

      l4 ([&]{trace << "no manifest prerequisite for target " << t;});
      return false;
    }

    recipe version_doc::
    apply (action a, target& xt) const
    {
      doc& t (static_cast<doc&> (xt));

      // Derive file names for the members.
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
      const path& f (t.path ());

      const scope& rs (t.root_scope ());
      const module& m (*rs.modules.lookup<module> (module::name));

      // Determine if anything needs to be updated.
      //
      // While we use the manifest file to decide whether we need to
      // regenerate the version file, the version itself we get from the
      // module (we checked above that manifest and version files are in the
      // same project).
      //
      // That is, unless we patched the snapshot information in, in which case
      // we have to compare the contents.
      //
      {
        auto p (execute_prerequisites (a, t, t.load_mtime ()));

        if (!p.first)
        {
          if (!m.version_patched || !exists (f))
            return p.second;

          try
          {
            ifdstream ifs (f, fdopen_mode::in, ifdstream::badbit);

            string s;
            getline (ifs, s);

            if (s == m.version.string_project ())
              return p.second;
          }
          catch (const io_error& e)
          {
            fail << "unable to read " << f << ": " << e;
          }
        }
      }

      if (verb >= 2)
        text << "cat >" << f;

      try
      {
        ofdstream ofs (f);
        ofs << m.version.string_project () << endl;
        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write " << f << ": " << e;
      }

      t.mtime (system_clock::now ());
      return target_state::changed;
    }
  }
}
