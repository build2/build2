// file      : libbuild2/bash/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bash/rule.hxx>

#include <cstring> // strlen(), strchr()

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/in/target.hxx>

#include <libbuild2/bash/target.hxx>
#include <libbuild2/bash/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace bash
  {
    using in::in;

    struct match_data
    {
      explicit
      match_data (const in_rule& r): rule (r) {}

      // The "for install" condition is signalled to us by install_rule when
      // it is matched for the update operation. It also verifies that if we
      // have already been executed, then it was for install.
      //
      // See cc::link_rule for a discussion of some subtleties in this logic.
      //
      optional<bool> for_install;

      const in_rule& rule;

      target_state
      operator() (action a, const target& t)
      {
        // Unless the outer install rule signalled that this is update for
        // install, signal back that we've performed plain update.
        //
        if (!for_install)
          for_install = false;

        //@@ TODO: need to verify all the modules we depend on are compatible
        //         with our for_install value, similar to cc::link_rule's
        //         append_libraries() (and which is the other half of the check
        //         in install_rule).

        return rule.perform_update (a, t);
      }
    };

    static_assert (sizeof (match_data) <= target::small_data_size,
                   "match data requires dynamic allocation");

    // in_rule
    //
    bool in_rule::
    match (action a, target& xt, const string& hint, match_extra&) const
    {
      tracer trace ("bash::in_rule::match");

      file& t (xt.as<file> ()); // Only registered for exe{} and bash{}.

      // Note that for bash{} and for exe{} with hint we match even if the
      // target does not depend on any modules (while it could have been
      // handled by the in module, that would require loading it).
      //
      bool fi (false);                             // Found in.
      bool fm (!hint.empty () || t.is_a<bash> ()); // Found module.
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
          continue;

        fi = fi || p.is_a<in> ();
        fm = fm || p.is_a<bash> ();
      }

      if (!fi)
        l4 ([&]{trace << "no in file prerequisite for target " << t;});

      if (!fm)
        l4 ([&]{trace << "no bash module prerequisite or hint for target "
                      << t;});

      // If we match, derive the file name early as recommended by the in
      // rule.
      //
      if (fi && fm)
        t.derive_path ();

      return fi && fm;
    }

    recipe in_rule::
    apply (action a, target& t) const
    {
      recipe r (rule::apply (a, t));

      if (a == perform_update_id)
      {
        // Note that for-install is signalled by install_rule and therefore
        // can only be relied upon during execute.
        //
        return match_data (*this);
      }

      return r;
    }

    prerequisite_target in_rule::
    search (action a,
            const target& t,
            const prerequisite_member& pm,
            include_type i) const
    {
      tracer trace ("bash::in_rule::search");

      // Handle import of installed bash{} modules.
      //
      if (i == include_type::normal && pm.proj () && pm.is_a<bash> ())
      {
        // We only need this during update.
        //
        if (a != perform_update_id)
          return nullptr;

        const prerequisite& p (pm.prerequisite);

        // Form the import path.
        //
        // Note that unless specified, we use the standard .bash extension
        // instead of going through the bash{} target type since this path is
        // not in our project (and thus no project-specific customization
        // apply).
        //
        string ext (p.ext ? *p.ext : "bash");
        path ip (dir_path (modules_install_dir (*p.proj)) / p.dir / p.name);

        if (!ext.empty ())
        {
          ip += '.';
          ip += ext;
        }

        // Search in PATH, similar to butl::path_search().
        //
        if (optional<string> s = getenv ("PATH"))
        {
          for (const char* b (s->c_str ()), *e;
               b != nullptr;
               b = (e != nullptr ? e + 1 : e))
          {
            e = strchr (b, path::traits_type::path_separator);

            // Empty path (i.e., a double colon or a colon at the beginning or
            // end of PATH) means search in the current dirrectory. We aren't
            // going to do that. Also silently skip invalid paths, stat()
            // errors, etc.
            //
            if (size_t n = (e != nullptr ? e - b : strlen (b)))
            {
              try
              {
                path ap (b, n);
                ap /= ip;
                ap.normalize ();

                timestamp mt (file_mtime (ap));

                if (mt != timestamp_nonexistent)
                {
                  // @@ Do we actually need _locked(), isn't path_mtime()
                  //    atomic?
                  //
                  auto rp (t.ctx.targets.insert_locked (bash::static_type,
                                                        ap.directory (),
                                                        dir_path () /* out */,
                                                        p.name,
                                                        ext,
                                                        target_decl::implied,
                                                        trace));

                  bash& pt (rp.first.as<bash> ());

                  // Only set path/mtime on first insertion.
                  //
                  if (rp.second)
                    pt.path_mtime (move (ap), mt);

                  // Save the length of the import path in auxuliary data. We
                  // use it in substitute_import() to infer the installation
                  // directory.
                  //
                  return prerequisite_target (&pt, i, ip.size ());
                }
              }
              catch (const invalid_path&) {}
              catch (const system_error&) {}
            }
          }
        }

        // Let standard search() handle it.
      }

      return rule::search (a, t, pm, i);
    }

    optional<string> in_rule::
    substitute (const location& l,
                action a,
                const target& t,
                const string& n,
                optional<uint64_t> flags,
                bool strict,
                const substitution_map* smap,
                const optional<string>& null) const
    {
      assert (!flags);

      return n.compare (0, 6, "import") == 0 && (n[6] == ' ' || n[6] == '\t')
        ? substitute_import (l, a, t, trim (string (n, 7)))
        : rule::substitute (l, a, t, n, nullopt, strict, smap, null);
    }

    string in_rule::
    substitute_import (const location& l,
                       action a,
                       const target& t,
                       const string& n) const
    {
      // Derive (relative) import path from the import name. And derive import
      // installed path from that by adding the .bash extension to the first
      // component.
      //
      path ip, iip;
      project_name pn;

      try
      {
        ip = path (n);

        if (ip.empty () || ip.simple () || ip.absolute ())
          throw invalid_path (n);

        if (ip.extension_cstring () == nullptr)
          ip += ".bash";

        ip.normalize ();

        auto b (ip.begin ()), e (ip.end ());

        try
        {
          pn = project_name (*b);
        }
        catch (const invalid_argument& e)
        {
          fail (l) << "invalid import path '" << n << "': " << e.what ();
        }

        char s (b++.separator ());

        iip = path (modules_install_dir (pn) + s) / path (b, e);

        // Strip the .bash extension from the project name in this path to
        // be able to compare it to paths inside the project (see below).
        //
        if (pn.extension () == "bash")
          ip = path (pn.base ("bash") + s) / path (b, e);
      }
      catch (const invalid_path&)
      {
        fail (l) << "invalid import path '" << n << "'";
      }

      // Look for a matching prerequisite.
      //
      const path* ap (nullptr);
      for (const prerequisite_target& pt: t.prerequisite_targets[a])
      {
        if (pt.target == nullptr || pt.adhoc ())
          continue;

        if (const bash* b = pt.target->is_a<bash> ())
        {
          const path& pp (b->path ());
          assert (!pp.empty ()); // Should have been assigned by update.

          // The simple "tail match" can be ambigous. Consider, for example,
          // the foo/bar.bash import path and /.../foo/bar.bash as well as
          // /.../x/foo/bar.bash prerequisites: they would both match.
          //
          // So the rule is the match must be from the project root directory
          // or from the installation directory for the import-installed
          // prerequisites.
          //
          // But we still do a simple match first since it can quickly weed
          // out candidates that cannot possibly match.

          // See if this is import-installed target (refer to search() above
          // for details).
          //
          if (size_t n = pt.data)
          {
            if (!pp.sup (iip))
              continue;

            // Both are normalized so we can compare the "tails".
            //
            const string& ps (pp.string ());
            const string& is (iip.string ());

            if (path::traits_type::compare (
                  ps.c_str () + ps.size () - n, n,
                  is.c_str (),                  is.size ()) == 0)
            {
              ap = &pp;
              break;
            }
            else
              continue;
          }

          if (const scope* rs = b->base_scope ().root_scope ())
          {
            if (!pp.sup (ip) || project (*rs) != pn)
              continue;

            const dir_path& d (pp.sub (rs->src_path ())
                               ? rs->src_path ()
                               : rs->out_path ());

            if (pp.leaf (d) == ip)
            {
              ap = &pp;
              break;
            }
            else
              continue;
          }

          fail (l) << "target " << *b << " is out of project nor imported";
        }
      }

      if (ap == nullptr)
        fail (l) << "unable to resolve import path " << ip;

      match_data& md (t.data<match_data> (a));
      assert (md.for_install);

      if (*md.for_install)
      {
        // For the installed case we assume the script and all its modules are
        // installed into the same location (i.e., the same bin/ directory)
        // and so we use the path relative to the script.
        //
        // BTW, the semantics of the source builtin in bash is to search in
        // PATH if it's a simple path (that is, does not contain directory
        // components) and then in the current working directory.
        //
        // So we have to determine the scripts's directory ourselves for which
        // we use the BASH_SOURCE array. Without going into the gory details,
        // the last element in this array is the script's path regardless of
        // whether we are in the script or (sourced) module (but it turned out
        // not to be what we need; see below).
        //
        // We also want to get the script's "real" directory even if it was
        // itself symlinked somewhere else. And this is where things get
        // hairy: we could use either realpath or readlink -f but neither is
        // available on Mac OS (there is readlink but it doesn't support the
        // -f option).
        //
        // One can get GNU readlink from Homebrew but it will be called
        // greadlink. Note also that for any serious development one will
        // probably be also getting newer bash from Homebrew since the system
        // one is stuck in the GPLv2 version 3.2.X era. So a bit of a mess.
        //
        // For now let's use readlink -f and see how it goes. If someone wants
        // to use/support their scripts on Mac OS, they have several options:
        //
        // 1. Install greadlink (coreutils) and symlink it as readlink.
        //
        // 2. Add the readlink function to their script that does nothing;
        //    symlinking scripts won't be supported but the rest should work
        //    fine.
        //
        // 3. Add the readlink function to their script that calls greadlink.
        //
        // 4. Add the readlink function to their script that implements the
        //    -f mode (or at least the part of it that we need). See the bash
        //    module tests for some examples.
        //
        // In the future we could automatically inject an implementation along
        // the (4) lines at the beginning of the script.
        //
        // Note also that we really, really want to keep the substitution a
        // one-liner since the import can be in an (indented) if-block, etc.,
        // and we still want the resulting scripts to be human-readable.
        //
        if (t.is_a<exe> ())
        {
          return
            "source \"$(dirname"
            " \"$(readlink -f"
            " \"${BASH_SOURCE[0]}\")\")/"
            + iip.string () + '"';
        }
        else
        {
          // Things turned out to be trickier for the installed modules: we
          // cannot juts use the script's path since it itself might not be
          // installed (import installed). So we have to use the importer's
          // path and calculate its "offset" to the installation directory.
          //
          dir_path d (t.dir.leaf (t.root_scope ().out_path ()));

          string o;
          for (auto i (d.begin ()), e (d.end ()); i != e; ++i)
            o += "../";

          // Here we don't use readlink since we assume nobody will symlink
          // the modules (or they will all be symlinked together).
          //
          return
            "source \"$(dirname"
            " \"${BASH_SOURCE[0]}\")/"
            + o + iip.string () + '"';
        }
      }
      else
        return "source " + ap->string ();
    }

    // install_rule
    //
    bool install_rule::
    match (action a, target& t) const
    {
      // We only want to handle installation if we are also the ones building
      // this target. So first run in's match().
      //
      return in_.sub_match (in_name_, update_id, a, t) &&
        file_rule::match (a, t);
    }

    recipe install_rule::
    apply (action a, target& t, match_extra& me) const
    {
      recipe r (file_rule::apply_impl (a, t, me));

      if (r == nullptr)
        return noop_recipe;

      if (a.operation () == update_id)
      {
        // Signal to the in rule that this is update for install. And if the
        // update has already been executed, verify it was done for install.
        //
        auto& md (t.data<match_data> (a.inner_action ()));

        if (md.for_install)
        {
          if (!*md.for_install)
            fail << "incompatible " << t << " build" <<
              info << "target already built not for install";
        }
        else
          md.for_install = true;
      }

      return r;
    }
  }
}
