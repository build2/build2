// file      : build2/cli/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cli/rule>

#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/algorithm>
#include <build2/filesystem>
#include <build2/diagnostics>

#include <build2/cli/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cli
  {
    // Figure out if name contains stem and, optionally, calculate prefix and
    // suffix.
    //
    static bool
    match_stem (const string& name, const string& stem,
                string* prefix = nullptr, string* suffix = nullptr)
    {
      size_t p (name.find (stem));

      if (p != string::npos)
      {
        if (prefix != nullptr)
          prefix->assign (name, 0, p);

        if (suffix != nullptr)
          suffix->assign (name, p + stem.size (), string::npos);

        return true;
      }

      return false;
    }

    match_result compile::
    match (slock& ml, action a, target& xt, const string&) const
    {
      tracer trace ("cli::compile::match");

      if (cli_cxx* pt = xt.is_a<cli_cxx> ())
      {
        // The cli.cxx{} group.
        //
        cli_cxx& t (*pt);

        // See if we have a .cli source file.
        //
        bool r (false);
        for (prerequisite_member p: group_prerequisite_members (ml, a, t))
        {
          if (p.is_a<cli> ())
          {
            // Check that the stem match.
            //
            if (!match_stem (t.name, p.name ()))
            {
              l4 ([&]{trace << ".cli file stem '" << p.name () << "' "
                            << "doesn't match target " << t;});
              return false;
            }

            r = true;
            break;
          }
        }

        if (!r)
        {
          l4 ([&]{trace << "no .cli source file for target " << t;});
          return r;
        }

        // If we still haven't figured out the member list, we can do
        // that now. Specifically, at this stage, no further changes to
        // cli.options are possible and we can determine whether the
        // --suppress-inline option is present.
        //
        if (t.h == nullptr)
        {
          t.h = &search<cxx::hxx> (t.dir, t.out, t.name, nullopt, nullptr);
          t.h->group = &t;

          t.c = &search<cxx::cxx> (t.dir, t.out, t.name, nullopt, nullptr);
          t.c->group = &t;

          if (!find_option ("--suppress-inline", t, "cli.options"))
          {
            t.i = &search<cxx::ixx> (t.dir, t.out, t.name, nullopt, nullptr);
            t.i->group = &t;
          }
        }

        return r;
      }
      else
      {
        // One of the ?xx{} members.
        //
        target& t (xt);

        // First see if we are already linked-up to the cli.cxx{} group. If
        // it is some other group, then we are definitely not a match.
        //
        if (t.group != nullptr)
          return t.group->is_a<cli_cxx> () != nullptr;

        // Check if there is a corresponding cli.cxx{} group.
        //
        cli_cxx* g (targets.find<cli_cxx> (t.dir, t.out, t.name));

        // If not or if it has no prerequisites (happens when we use it to
        // set cli.options) and this target has a cli{} prerequisite, then
        // synthesize the group.
        //
        if (g == nullptr || !g->has_prerequisites ())
        {
          for (prerequisite_member p: group_prerequisite_members (ml, a, t))
          {
            if (p.is_a<cli> ())
            {
              // Check that the stems match.
              //
              if (match_stem (t.name, p.name ()))
              {
                if (g == nullptr)
                  g = &targets.insert<cli_cxx> (t.dir, t.out, t.name, trace);

                g->prerequisites.push_back (p.as_prerequisite ());
              }
              else
                l4 ([&]{trace << ".cli file stem '" << p.name () << "' "
                              << "doesn't match target " << t;});
              break;
            }
          }
        }

        if (g != nullptr)
        {
          // Resolve the group's members. This should link us up to the
          // group.
          //
          resolve_group_members (ml, a, *g);

          // For ixx{}, verify it is part of the group.
          //
          if (t.is_a<cxx::ixx> () && g->i == nullptr)
          {
            l4 ([&]{trace << "generation of inline file " << t
                          << " is disabled with --suppress-inline";});
            g = nullptr;
          }
        }

        assert (t.group == g);
        return g != nullptr;
      }
    }

    recipe compile::
    apply (slock& ml, action a, target& xt) const
    {
      if (cli_cxx* pt = xt.is_a<cli_cxx> ())
      {
        cli_cxx& t (*pt);

        // Derive file names for the members.
        //
        t.h->derive_path ();
        t.c->derive_path ();
        if (t.i != nullptr)
          t.i->derive_path ();

        // Inject dependency on the output directory.
        //
        inject_fsdir (ml, a, t);

        // Search and match prerequisite members.
        //
        search_and_match_prerequisite_members (ml, a, t);

        switch (a)
        {
        case perform_update_id: return &perform_update;
        case perform_clean_id: return &perform_clean;
        default: return noop_recipe; // Configure update.
        }
      }
      else
      {
        cli_cxx& g (*static_cast<cli_cxx*> (xt.group));
        build2::match (ml, a, g);
        return group_recipe; // Execute the group's recipe.
      }
    }

    static void
    append_extension (cstrings& args,
                      path_target& t,
                      const char* option,
                      const char* default_extension)
    {
      // Should have been figured out in apply().
      //
      const optional<string>& e (*t.ext_);
      assert (e);

      if (*e != default_extension)
      {
        // CLI needs the extension with the leading dot (unless it is empty)
        // while we store the extension without. But if there is an extension,
        // then we can get it (with the dot) from the file name.
        //
        args.push_back (option);
        args.push_back (e->empty ()
                        ? e->c_str ()
                        : t.path ().extension_cstring () - 1);
      }
    }

    target_state compile::
    perform_update (action a, target& xt)
    {
      cli_cxx& t (static_cast<cli_cxx&> (xt));

      // Update prerequisites and determine if any relevant ones render us
      // out-of-date. Note that currently we treat all the prerequisites
      // as potentially affecting the result (think prologues/epilogues,
      // etc).
      //
      cli* s;
      {
        auto p (execute_prerequisites<cli> (a, t, t.mtime ()));

        if ((s = p.first) == nullptr)
          return p.second;
      }

      // Translate paths to relative (to working directory). This
      // results in easier to read diagnostics.
      //
      path relo (relative (t.dir));
      path rels (relative (s->path ()));

      const scope& rs (t.root_scope ());

      const process_path& cli (cast<process_path> (rs["cli.path"]));

      cstrings args {cli.recall_string ()};

      // See if we need to pass --output-{prefix,suffix}
      //
      string prefix, suffix;
      match_stem (t.name, s->name, &prefix, &suffix);

      if (!prefix.empty ())
      {
        args.push_back ("--output-prefix");
        args.push_back (prefix.c_str ());
      }

      if (!suffix.empty ())
      {
        args.push_back ("--output-suffix");
        args.push_back (suffix.c_str ());
      }

      // See if we need to pass any --?xx-suffix options.
      //
      append_extension (args, *t.h, "--hxx-suffix", "hxx");
      append_extension (args, *t.c, "--cxx-suffix", "cxx");
      if (t.i != nullptr)
        append_extension (args, *t.i, "--ixx-suffix", "ixx");

      append_options (args, t, "cli.options");

      if (!relo.empty ())
      {
        args.push_back ("-o");
        args.push_back (relo.string ().c_str ());
      }

      args.push_back (rels.string ().c_str ());
      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "cli " << *s;

      try
      {
        process pr (cli, args.data ());

        if (!pr.wait ())
          throw failed ();

        t.mtime (system_clock::now ());
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child ())
          exit (1);

        throw failed ();
      }

      return target_state::changed;
    }

    target_state compile::
    perform_clean (action a, target& xt)
    {
      cli_cxx& t (static_cast<cli_cxx&> (xt));

      // The reverse order of update: first delete the files, then clean
      // prerequisites. Also update timestamp in case there are operations
      // after us that could use the information.
      //
      // @@ Can't we use clean_extra() for this?
      //
      bool r (false);

      if (t.i != nullptr)
        r = rmfile (t.i->path (), *t.i) || r;
      r = rmfile (t.c->path (), *t.c) || r;
      r = rmfile (t.h->path (), *t.h) || r;

      t.mtime (timestamp_nonexistent);

      target_state ts (r ? target_state::changed : target_state::unchanged);

      // Clean prerequisites.
      //
      ts |= reverse_execute_prerequisites (a, t);

      return ts;
    }
  }
}
