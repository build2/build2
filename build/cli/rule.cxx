// file      : build/cli/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cli/rule>

#include <butl/process>

#include <build/scope>
#include <build/target>
#include <build/context>
#include <build/algorithm>
#include <build/diagnostics>

#include <build/cli/target>

#include <build/config/utility>

using namespace std;
using namespace butl;

namespace build
{
  namespace cli
  {
    using config::append_options;

    match_result compile::
    match (action a, target& xt, const std::string&) const
    {
      tracer trace ("cli::compile::match");

      if (cli_cxx* pt = xt.is_a<cli_cxx> ())
      {
        cli_cxx& t (*pt);

        // See if we have a .cli source file.
        //
        match_result r;
        for (prerequisite_member p: group_prerequisite_members (a, t))
        {
          if (p.is_a<cli> ())
          {
            //@@ Need to verify input and output stems match.
            r = p;
            break;
          }
        }

        if (!r)
        {
          level3 ([&]{trace << "no .cli source file for target " << t;});
          return nullptr;
        }

        // If we still haven't figured out the member list, we can do
        // that now. Specifically, at this stage no further changes to
        // cli.options are possible and we can determine whether the
        // --suppress-inline option is present.
        //
        if (t.h () == nullptr)
        {
          cxx::hxx& h (search<cxx::hxx> (t.dir, t.name, nullptr, nullptr));
          h.group = &t;
          t.h (h);

          cxx::cxx& c (search<cxx::cxx> (t.dir, t.name, nullptr, nullptr));
          c.group = &t;
          t.c (c);

          bool inl (true);
          if (auto val = t["cli.options"])
          {
            for (const name& n: val.template as<const list_value&> ())
            {
              if (n.value == "--suppress-inline")
              {
                inl = false;
                break;
              }
            }
          }

          if (inl)
          {
            cxx::ixx& i (search<cxx::ixx> (t.dir, t.name, nullptr, nullptr));
            i.group = &t;
            t.i (i);
          }
        }

        return r;
      }
      else
      {
        // One of the ?xx{} members.
        //
        target& t (xt);

        // First see if we are already linked-up to the cli.cxx{} group.
        // If it is some other group, then we are definitely not a match.
        //
        if (t.group != nullptr)
          return t.group->is_a<cli_cxx> ();

        // Then see if there is a corresponding cli.cxx{} group.
        //
        cli_cxx* g (targets.find<cli_cxx> (t.dir, t.name));

        // Finally, if this target has a cli{} prerequisite, synthesize
        // the group.
        //
        if (g == nullptr)
        {
          for (prerequisite_member p: group_prerequisite_members (a, t))
          {
            if (p.is_a<cli> ()) // @@ Need to check that stems match.
            {
              g = &targets.insert<cli_cxx> (t.dir, t.name, trace);
              g->prerequisites.emplace_back (p.as_prerequisite (trace));
              break;
            }
          }
        }

        if (g != nullptr)
        {
          // Resolve the group's members. This should link us up to
          // the group.
          //
          resolve_group_members (a, *g);

          // For ixx{}, verify it is part of the group.
          //
          if (t.is_a<cxx::ixx> () && g->i () == nullptr)
          {
            level3 ([&]{trace << "generation of inline file " << t
                              << " is disabled with --suppress-inline";});
            g = nullptr;
          }
        }

        assert (t.group == g);
        return g;
      }
    }

    recipe compile::
    apply (action a, target& xt, const match_result& mr) const
    {
      if (cli_cxx* pt = xt.is_a<cli_cxx> ())
      {
        cli_cxx& t (*pt);

        // Derive file names for the members.
        //
        t.h ()->derive_path ();
        t.c ()->derive_path ();
        if (t.i () != nullptr)
          t.i ()->derive_path ();

        // Inject dependency on the output directory.
        //
        inject_parent_fsdir (a, t);

        // Search and match prerequisites.
        //
        switch (a.operation ())
        {
        case default_id:
        case update_id: search_and_match (a, t); break;
        case clean_id: search_and_match (a, t, t.dir); break;
        default: assert (false); // We didn't register for this.
        }

        switch (a)
        {
        case perform_update_id: return &perform_update;
        case perform_clean_id: return &perform_clean;
        default: return default_recipe; // Forward to prerequisites.
        }
      }
      else
      {
        cli_cxx& g (*static_cast<cli_cxx*> (mr.target));
        build::match (a, g);
        return &delegate;
      }
    }

    static void
    append_extension (vector<const char*>& args,
                      path_target& t,
                      const char* opt,
                      const char* def)
    {
      assert (t.ext != nullptr); // Should have been figured out in apply().

      if (*t.ext != def)
      {
        // CLI needs the extension with the leading dot (unless it is empty)
        // while we store the extension without. But if there is an extension,
        // then we can get it (with the dot) from the file name.
        //
        args.push_back (opt);
        args.push_back (t.ext->empty ()
                        ? t.ext->c_str ()
                        : t.path ().extension () - 1);
      }
    }

    target_state compile::
    perform_update (action a, target& xt)
    {
      cli_cxx& t (static_cast<cli_cxx&> (xt));

      // Execute our prerequsites and check if we are out of date.
      //
      cli* s (execute_prerequisites<cli> (a, t, t.h ()->mtime ()));

      target_state ts;

      if (s == nullptr)
        ts = target_state::unchanged;
      else
      {
        // Translate source path to relative (to working directory). This
        // results in easier to read diagnostics.
        //
        path relo (relative (t.dir));
        path rels (relative (s->path ()));

        scope& rs (t.root_scope ());
        const string& cli (rs["config.cli"].as<const string&> ());

        vector<const char*> args {cli.c_str ()};

        // See if we need to pass any --?xx-suffix options.
        //
        append_extension (args, *t.h (), "--hxx-suffix", "hxx");
        append_extension (args, *t.c (), "--cxx-suffix", "cxx");
        if (t.i () != nullptr)
          append_extension (args, *t.i (), "--ixx-suffix", "ixx");

        append_options (args, t, "cli.options");

        if (!relo.empty ())
        {
          args.push_back ("-o");
          args.push_back (relo.string ().c_str ());
        }

        args.push_back (rels.string ().c_str ());
        args.push_back (nullptr);

        if (verb)
          print_process (args);
        else
          text << "cli " << *s;

        try
        {
          process pr (args.data ());

          if (!pr.wait ())
            throw failed ();

          timestamp s (system_clock::now ());

          // Update member timestamps.
          //
          t.h ()->mtime (s);
          t.c ()->mtime (s);
          if (t.i () != nullptr)
            t.i ()->mtime (s);

          ts = target_state::changed;
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << args[0] << ": " << e.what ();

          if (e.child ())
            exit (1);

          throw failed ();
        }
      }

      // Update member recipes. Without that the state update below
      // won't stick.
      //
      if (!t.h ()->recipe (a))
        t.h ()->recipe (a, &delegate);

      if (!t.c ()->recipe (a))
        t.c ()->recipe (a, &delegate);

      if (t.i () != nullptr && !t.i ()->recipe (a))
        t.i ()->recipe (a, &delegate);

      // Update member states.
      //
      t.h ()->state = ts;
      t.c ()->state = ts;
      if (t.i () != nullptr)
        t.i ()->state = ts;

      return ts;
    }

    target_state compile::
    perform_clean (action a, target& xt)
    {
      cli_cxx& t (static_cast<cli_cxx&> (xt));

      // The reverse order of update: first delete the files, then clean
      // prerequisites. Also update timestamp in case there are operations
      // after us that could use the information.
      //
      //
      bool r (false);

      if (t.i () != nullptr)
      {
        r = rmfile (t.i ()->path (), *t.i ()) || r;
        t.i ()->mtime (timestamp_nonexistent);
      }

      r = rmfile (t.c ()->path (), *t.c ()) || r;
      t.c ()->mtime (timestamp_nonexistent);

      r = rmfile (t.h ()->path (), *t.h ()) || r;
      t.h ()->mtime (timestamp_nonexistent);

      // Clean prerequisites.
      //
      target_state ts (reverse_execute_prerequisites (a, t));

      return r ? target_state::changed : ts;
    }

    target_state compile::
    delegate (action a, target& t)
    {
      // Delegate to our group.
      //
      return execute (a, *t.group);
    }
  }
}
