// file      : build2/cli/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cli/rule.hxx>

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/context.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/cli/target.hxx>

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

    bool compile_rule::
    match (action a, target& t, const string&) const
    {
      tracer trace ("cli::compile_rule::match");

      // Find the .cli source file.
      //
      auto find = [&trace, a, &t] (auto&& r) -> optional<prerequisite_member>
      {
        for (prerequisite_member p: r)
        {
          // If excluded or ad hoc, then don't factor it into our tests.
          //
          if (include (a, t, p) != include_type::normal)
            continue;

          if (p.is_a<cli> ())
          {
            // Check that the stem match.
            //
            if (match_stem (t.name, p.name ()))
              return p;

            l4 ([&]{trace << ".cli file stem '" << p.name () << "' "
                          << "doesn't match target " << t;});
          }
        }

        return nullopt;
      };

      if (cli_cxx* pt = t.is_a<cli_cxx> ())
      {
        // The cli.cxx{} group.
        //
        cli_cxx& t (*pt);

        // See if we have a .cli source file.
        //
        if (!find (group_prerequisite_members (a, t)))
        {
          l4 ([&]{trace << "no .cli source file for target " << t;});
          return false;
        }

        // Figure out the member list.
        //
        // At this stage, no further changes to cli.options are possible and
        // we can determine whether the --suppress-inline option is present.
        //
        // Passing the group as a "reference target" is a bit iffy,
        // conceptually.
        //
        t.h = &search<cxx::hxx> (t, t.dir, t.out, t.name);
        t.c = &search<cxx::cxx> (t, t.dir, t.out, t.name);
        t.i = find_option ("--suppress-inline", t, "cli.options")
          ? nullptr
          : &search<cxx::ixx> (t, t.dir, t.out, t.name);

        return true;
      }
      else
      {
        // One of the ?xx{} members.
        //

        // Check if there is a corresponding cli.cxx{} group.
        //
        const cli_cxx* g (targets.find<cli_cxx> (t.dir, t.out, t.name));

        // If not or if it has no prerequisites (happens when we use it to
        // set cli.options) and this target has a cli{} prerequisite, then
        // synthesize the dependency.
        //
        if (g == nullptr || !g->has_prerequisites ())
        {
          if (optional<prerequisite_member> p = find (
                prerequisite_members (a, t)))
          {
            if (g == nullptr)
              g = &targets.insert<cli_cxx> (t.dir, t.out, t.name, trace);

            g->prerequisites (prerequisites {p->as_prerequisite ()});
          }
        }

        if (g == nullptr)
          return false;

        // For ixx{}, verify it is part of the group (i.e., not disabled
        // via --suppress-inline).
        //
        if (t.is_a<cxx::ixx> () &&
            find_option ("--suppress-inline", *g, "cli.options"))
          return false;

        t.group = g;
        return true;
      }
    }

    recipe compile_rule::
    apply (action a, target& xt) const
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
        inject_fsdir (a, t);

        // Match prerequisites.
        //
        match_prerequisite_members (a, t);

        switch (a)
        {
        case perform_update_id: return &perform_update;
        case perform_clean_id:  return &perform_clean_group; // Standard impl.
        default:                return noop_recipe; // Configure/dist update.
        }
      }
      else
      {
        const cli_cxx& g (xt.group->as<cli_cxx> ());
        build2::match (a, g);
        return group_recipe; // Execute the group's recipe.
      }
    }

    static void
    append_extension (cstrings& args,
                      const path_target& t,
                      const char* option,
                      const char* default_extension)
    {
      const string* e (t.ext ());
      assert (e != nullptr); // Should have been figured out in apply().

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

    target_state compile_rule::
    perform_update (action a, const target& xt)
    {
      const cli_cxx& t (xt.as<cli_cxx> ());

      // Update prerequisites and determine if any relevant ones render us
      // out-of-date. Note that currently we treat all the prerequisites
      // as potentially affecting the result (think prologues/epilogues,
      // etc).
      //

      // The rule has been matched which means the members should be resolved
      // and paths assigned.
      //
      auto pr (
        execute_prerequisites<cli> (
          a, t, t.load_mtime (t.h->path ())));

      if (pr.first)
        return *pr.first;

      const cli& s (pr.second);

      // Translate paths to relative (to working directory). This
      // results in easier to read diagnostics.
      //
      path relo (relative (t.dir));
      path rels (relative (s.path ()));

      const scope& rs (t.root_scope ());

      const process_path& cli (cast<process_path> (rs["cli.path"]));

      cstrings args {cli.recall_string ()};

      // See if we need to pass --output-{prefix,suffix}
      //
      string prefix, suffix;
      match_stem (t.name, s.name, &prefix, &suffix);

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
        text << "cli " << s;

      run (cli, args);

      t.mtime (system_clock::now ());
      return target_state::changed;
    }
  }
}
