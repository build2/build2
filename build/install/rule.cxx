// file      : build/install/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/install/rule>

#include <butl/process>
#include <butl/filesystem>

#include <build/scope>
#include <build/target>
#include <build/algorithm>
#include <build/diagnostics>

#include <build/config/utility>

using namespace std;
using namespace butl;

namespace build
{
  namespace install
  {
    // Lookup the install or install.* variable. Return NULL if
    // not found or if the value is the special 'false' name (which
    // means do not install). T is either scope or target.
    //
    template <typename T>
    static const dir_path*
    lookup (T& t, const string& var)
    {
      auto l (t[var]);

      if (!l)
        return nullptr;

      const dir_path& r (as<dir_path> (*l));
      return r.simple () && r.string () == "false" ? nullptr : &r;
    }

    match_result rule::
    match (action a, target& t, const std::string&) const
    {
      // First determine if this target should be installed (called
      // "installable" for short).
      //
      match_result mr (t, lookup (t, "install") != nullptr);

      // If this is the update pre-operation, change the recipe action
      // to (update, 0) (i.e., "unconditional update").
      //
      if (mr.bvalue && a.operation () == update_id)
        mr.recipe_action = action (a.meta_operation (), update_id);

      return mr;
    }

    recipe rule::
    apply (action a, target& t, const match_result& mr) const
    {
      if (!mr.bvalue) // Not installable.
        return noop_recipe;

      // Ok, if we are here, then this means:
      //
      // 1. This target is installable.
      // 2. The action is either
      //    a. (perform, install, 0) or
      //    b. (*, update, install)
      //
      // In both cases, the next step is to search, match, and collect
      // all the installable prerequisites.
      //
      // @@ Perhaps if [noinstall] will be handled by the
      // group_prerequisite_members machinery, then we can just
      // run standard search_and_match()? Will need an indicator
      // that it was forced (e.g., [install]) for filter() below.
      //
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // @@ This is where we will handle [noinstall].
        //

        // Let a customized rule have its say.
        //
        // @@ This will be skipped if forced with [install].
        //
        if (!filter (a, t, p))
          continue;

        target& pt (p.search ());
        build::match (a, pt);

        // If the matched rule returned noop_recipe, then the target
        // state will be set to unchanged as an optimization. Use this
        // knowledge to optimize things on our side as well since this
        // will help a lot in case of any static installable content
        // (headers, documentation, etc).
        //
        if (pt.state () != target_state::unchanged)
          t.prerequisite_targets.push_back (&pt);
        else
          unmatch (a, pt); // No intent to execute.
      }

      // This is where we diverge depending on the operation. In the
      // update pre-operation, we need to make sure that this target
      // as well as all its installable prerequisites are up to date.
      //
      if (a.operation () == update_id)
      {
        // Save the prerequisite targets that we found since the
        // call to match_delegate() below will wipe them out.
        //
        target::prerequisite_targets_type p;

        if (!t.prerequisite_targets.empty ())
          p.swap (t.prerequisite_targets);

        // Find the "real" update rule, that is, the rule that would
        // have been found if we signalled that we do not match from
        // match() above.
        //
        recipe d (match_delegate (a, t).first);

        // If we have no installable prerequsites, then simply redirect
        // to it.
        //
        if (p.empty ())
          return d;

        // Ok, the worst case scenario: we need to cause update of
        // prerequisite targets and also delegate to the real update.
        //
        return [pt = move (p), dr = move (d)]
          (action a, target& t) mutable -> target_state
        {
          // Do the target update first.
          //
          target_state r (execute_delegate (dr, a, t));

          // Swap our prerequisite targets back in and execute.
          //
          t.prerequisite_targets.swap (pt);
          r |= execute_prerequisites (a, t);
          pt.swap (t.prerequisite_targets); // In case we get re-executed.

          return r;
        };
      }
      else
        return &perform_install;
    }

    struct install_dir
    {
      dir_path dir;
      string cmd; //@@ VAR type
      const_strings_value options {nullptr};
      string mode;
      string dir_mode;
    };

    // install -d <dir>
    //
    static void
    install (const install_dir& base, const dir_path& d)
    {
      path reld (relative (d));

      cstrings args {base.cmd.c_str (), "-d"};

      if (base.options.d != nullptr) //@@ VAR
        config::append_options (args, base.options);

      args.push_back ("-m");
      args.push_back (base.dir_mode.c_str ());
      args.push_back (reld.string ().c_str ());
      args.push_back (nullptr);

      if (verb)
        print_process (args);
      else
        text << "install " << d;

      try
      {
        process pr (args.data ());

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    // install <file> <dir>
    //
    static void
    install (const install_dir& base, file& t)
    {
      path reld (relative (base.dir));
      path relf (relative (t.path ()));

      cstrings args {base.cmd.c_str ()};

      if (base.options.d != nullptr) //@@ VAR
        config::append_options (args, base.options);

      args.push_back ("-m");
      args.push_back (base.mode.c_str ());
      args.push_back (relf.string ().c_str ());
      args.push_back (reld.string ().c_str ());
      args.push_back (nullptr);

      if (verb)
        print_process (args);
      else
        text << "install " << t;

      try
      {
        process pr (args.data ());

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    // Resolve installation directory name to absolute directory path,
    // creating leading directories as necessary.
    //
    static install_dir
    resolve (scope& s, dir_path d, const string* var = nullptr)
    {
      install_dir r;

      if (d.absolute ())
      {
        d.normalize ();

        // Make sure it already exists (this will normally be
        // install.root with everything else defined in term of it).
        //
        if (!dir_exists (d))
          fail << "installation directory " << d << " does not exist";
      }
      else
      {
        // If it is relative, then the first component is treated
        // as the installation directory name, e.g., bin, sbin, lib,
        // etc. Look it up and recurse.
        //
        const string& sn (*d.begin ());
        const string var ("install." + sn);
        if (const dir_path* dn = lookup (s, var))
        {
          r = resolve (s, *dn, &var);
          d = r.dir / dir_path (++d.begin (), d.end ());
          d.normalize ();

          if (!dir_exists (d))
            install (r, d); // install -d
        }
        else
          fail << "unknown installation directory name " << sn <<
            info << "did you forget to specify config." << var << "?";
      }

      r.dir = move (d);

      // Override components in install_dir if we have our own.
      //
      if (var != nullptr)
      {
        if (auto l = s[*var + ".cmd"]) r.cmd = as<string> (*l);
        if (auto l = s[*var + ".mode"]) r.mode = as<string> (*l);
        if (auto l = s[*var + ".dir_mode"]) r.dir_mode = as<string> (*l);
        if (auto l = s[*var + ".options"]) r.options = as<strings> (*l);
      }

      // Set defaults for unspecified components.
      //
      if (r.cmd.empty ()) r.cmd = "install";
      if (r.mode.empty ()) r.mode = "644";
      if (r.dir_mode.empty ()) r.dir_mode = "755";

      return r;
    }

    target_state rule::
    perform_install (action a, target& t)
    {
      file& ft (static_cast<file&> (t));
      assert (!ft.path ().empty ()); // Should have been assigned by update.

      // First handle installable prerequisites.
      //
      target_state r (execute_prerequisites (a, t));

      // Resolve and, if necessary, create target directory.
      //
      install_dir d (
        resolve (t.base_scope (),
                 as<dir_path> (*t["install"]))); // We know it's there.

      // Override mode if one was specified.
      //
      if (auto l = t["install.mode"])
        d.mode = as<string> (*l);

      install (d, ft);
      return (r |= target_state::changed);
    }
  }
}
