// file      : build2/install/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/install/rule>

#include <butl/filesystem> // dir_exists(), file_exists()

#include <build2/scope>
#include <build2/target>
#include <build2/algorithm>
#include <build2/filesystem>
#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
{
  namespace install
  {
    // Lookup the install or install.* variable. Return NULL if not found or
    // if the value is the special 'false' name (which means do not install).
    // T is either scope or target.
    //
    template <typename P, typename T>
    static const P*
    lookup_install (T& t, const string& var)
    {
      auto l (t[var]);

      if (!l)
        return nullptr;

      const P& r (cast<P> (l));
      return r.simple () && r.string () == "false" ? nullptr : &r;
    }

    // alias_rule
    //
    match_result alias_rule::
    match (action, target&, const string&) const
    {
      return true;
    }

    recipe alias_rule::
    apply (action a, target& t) const
    {
      tracer trace ("install::alias_rule::apply");

      for (prerequisite p: group_prerequisites (t))
      {
        target& pt (search (p));

        // Check if this prerequisite is explicitly "not installable",
        // that is, there is the 'install' variable and its value is
        // false.
        //
        // At first, this might seem redundand since we could have let
        // the file_rule below take care of it. The nuance is this: this
        // prerequsite can be in a different subproject that hasn't loaded
        // the install module (and therefore has no file_rule registered).
        // The typical example would be the 'tests' subproject.
        //
        // Note: not the same as lookup() above.
        //
        auto l (pt["install"]);

        if (l && cast<path> (l).string () == "false")
        {
          l5 ([&]{trace << "ignoring " << pt;});
          continue;
        }

        build2::match (a, pt);
        t.prerequisite_targets.push_back (&pt);
      }

      return default_recipe;
    }

    // file_rule
    //
    struct match_data
    {
      bool install;
    };

    static_assert (sizeof (match_data) <= target::data_size,
                   "insufficient space");

    match_result file_rule::
    match (action a, target& t, const string&) const
    {
      // First determine if this target should be installed (called
      // "installable" for short).
      //
      match_data md {lookup_install<path> (t, "install") != nullptr};
      match_result mr (true);

      if (a.operation () == update_id)
      {
        // If this is the update pre-operation and the target is installable,
        // change the recipe action to (update, 0) (i.e., "unconditional
        // update") so that we don't get matched for its prerequisites.
        //
        if (md.install)
          mr.recipe_action = action (a.meta_operation (), update_id);
        else
          // Otherwise, signal that we don't match so that some other rule can
          // take care of it.
          //
          return false;
      }

      t.data (md); // Save the data in the target's auxilary storage.
      return mr;
    }

    target* file_rule::
    filter (action, target& t, prerequisite_member p) const
    {
      target& pt (p.search ());
      return pt.in (t.root_scope ()) ? &pt : nullptr;
    }

    recipe file_rule::
    apply (action a, target& t) const
    {
      match_data md (move (t.data<match_data> ()));
      t.clear_data (); // In case delegated-to rule also uses aux storage.

      if (!md.install) // Not installable.
        return noop_recipe;

      // Ok, if we are here, then this means:
      //
      // 1. This target is installable.
      // 2. The action is either
      //    a. (perform, [un]install, 0) or
      //    b. (*, update, [un]install)
      //
      // In both cases, the next step is to search, match, and collect all the
      // installable prerequisites.
      //
      // @@ Perhaps if [noinstall] will be handled by the
      // group_prerequisite_members machinery, then we can just
      // run standard search_and_match()? Will need an indicator
      // that it was forced (e.g., [install]) for filter() below.
      //
      auto r (group_prerequisite_members (a, t));
      for (auto i (r.begin ()); i != r.end (); ++i)
      {
        prerequisite_member p (*i);

        // Ignore unresolved targets that are imported from other projects.
        // We are definitely not installing those.
        //
        if (p.proj () != nullptr)
          continue;

        // Let a customized rule have its say.
        //
        target* pt (filter (a, t, p));
        if (pt == nullptr)
          continue;

        // See if we were explicitly instructed not to touch this target.
        //
        auto l ((*pt)["install"]);
        if (l && cast<path> (l).string () == "false")
          continue;

        build2::match (a, *pt);

        // If the matched rule returned noop_recipe, then the target
        // state will be set to unchanged as an optimization. Use this
        // knowledge to optimize things on our side as well since this
        // will help a lot in case of any static installable content
        // (headers, documentation, etc).
        //
        if (pt->state () != target_state::unchanged)
          t.prerequisite_targets.push_back (pt);
        else
          unmatch (a, *pt); // No intent to execute.

        // Skip members of ad hoc groups. We handle them explicitly below.
        //
        if (pt->adhoc_group ())
          i.leave_group ();
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
        recipe d (match_delegate (a, t, *this).first);

        // If we have no installable prerequisites, then simply redirect
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
      else if (a.operation () == install_id)
        return [this] (action a, target& t) {return perform_install (a, t);};
      else
        return [this] (action a, target& t) {return perform_uninstall (a, t);};
    }

    void file_rule::
    install_extra (file&, const install_dir&) const {}

    bool file_rule::
    uninstall_extra (file&, const install_dir&) const {return false;}

    struct install_dir
    {
      dir_path dir;

      // If not NULL, then point to the corresponding install.* value.
      //
      const string*  sudo     = nullptr;
      const path*    cmd      = nullptr;
      const strings* options  = nullptr;
      const string*  mode     = nullptr;
      const string*  dir_mode = nullptr;

      explicit
      install_dir (dir_path d = dir_path ()): dir (move (d)) {}

      install_dir (dir_path d, const install_dir& b)
          : dir (move (d)),
            sudo (b.sudo),
            cmd (b.cmd),
            options (b.options),
            mode (b.mode),
            dir_mode (b.dir_mode) {}
    };

    using install_dirs = vector<install_dir>;

    // Calculate a subdirectory based on l's location (*.subdirs) and if not
    // empty add it to install_dirs. Return the new last element.
    //
    static install_dir&
    resolve_subdir (install_dirs& rs, target& t, scope& s, const lookup& l)
    {
      // Find the scope from which this value came and use as a base
      // to calculate the subdirectory.
      //
      for (const scope* p (&s); p != nullptr; p = p->parent_scope ())
      {
        if (l.belongs (*p)) // Ok since no target/type in lookup.
        {
          // The target can be in out or src.
          //
          const dir_path& d (t.out_dir ().leaf (p->out_path ()));

          // Add it as another leading directory rather than modifying
          // the last one directly; somehow, it feels right.
          //
          if (!d.empty ())
            rs.emplace_back (rs.back ().dir / d, rs.back ());
          break;
        }
      }

      return rs.back ();
    }

    // Resolve installation directory name to absolute directory path. Return
    // all the super-directories leading up to the destination (last).
    //
    static install_dirs
    resolve (target& t, dir_path d, const string* var = nullptr)
    {
      install_dirs rs;

      if (d.absolute ())
        rs.emplace_back (move (d.normalize ()));
      else
      {
        // If it is relative, then the first component is treated as the
        // installation directory name, e.g., bin, sbin, lib, etc. Look it
        // up and recurse.
        //
        const string& sn (*d.begin ());
        const string var ("install." + sn);
        if (const dir_path* dn =
            lookup_install<dir_path> (t.base_scope (), var))
        {
          if (dn->empty ())
            fail << "empty installation directory for name " << sn <<
              info << "did you specified empty config." << var << "?";

          rs = resolve (t, *dn, &var);
          d = rs.back ().dir / dir_path (++d.begin (), d.end ());
          rs.emplace_back (move (d.normalize ()), rs.back ());
        }
        else
          fail << "unknown installation directory name '" << sn << "'" <<
            info << "did you forget to specify config." << var << "?";
      }

      install_dir* r (&rs.back ());
      scope& s (t.base_scope ());

      // Override components in install_dir if we have our own.
      //
      if (var != nullptr)
      {
        if (auto l = s[*var + ".sudo"])     r->sudo     = &cast<string> (l);
        if (auto l = s[*var + ".cmd"])      r->cmd      = &cast<path> (l);
        if (auto l = s[*var + ".mode"])     r->mode     = &cast<string> (l);
        if (auto l = s[*var + ".dir_mode"]) r->dir_mode = &cast<string> (l);
        if (auto l = s[*var + ".options"])  r->options  = &cast<strings> (l);

        if (auto l = s[*var + ".subdirs"])
        {
          if (cast<bool> (l))
            r = &resolve_subdir (rs, t, s, l);
        }
      }

      // Set globals for unspecified components.
      //
      if (r->sudo == nullptr)
        r->sudo = cast_null<string> (s["config.install.sudo"]);

      if (r->cmd == nullptr)
        r->cmd = &cast<path> (s["config.install.cmd"]);

      if (r->options == nullptr)
        r->options = cast_null<strings> (s["config.install.options"]);

      if (r->mode == nullptr)
        r->mode = &cast<string> (s["config.install.mode"]);

      if (r->dir_mode == nullptr)
        r->dir_mode = &cast<string> (s["config.install.dir_mode"]);

      return rs;
    }

    // On Windows we use MSYS2 install.exe and MSYS2 by default ignores
    // filesystem permissions (noacl mount option). And this means, for
    // example, that .exe that we install won't be runnable by Windows (MSYS2
    // itself will still run them since it recognizes the file extension).
    //
    // The way we work around this (at least in our distribution of the MSYS2
    // tools) is by changing the mount option for cygdrives (/c, /d, etc) to
    // acl. But that's not all: we also have to install via a path that "hits"
    // one of those mount points, c:\foo won't work, we have to use /c/foo.
    // So this function translates an absolute Windows path to its MSYS
    // representation.
    //
    // Note that we return the result as a string, not dir_path since path
    // starting with / are illegal on Windows. Also note that the result
    // doesn't have the trailing slash.
    //
    static string
    msys_path (const dir_path& d)
    {
      assert (d.absolute ());
      string s (d.representation ());

      // First replace ':' with the drive letter (so the path is no longer
      // absolute) but postpone setting the first character to / until we are
      // a string.
      //
      s[1] = lcase (s[0]);
      s = dir_path (move (s)).posix_string ();
      s[0] = '/';

      return s;
    }

    // install -d <dir>
    //
    // If verbose is false, then only print the command at verbosity level 2
    // or higher.
    //
    static void
    install_d (const install_dir& base, const dir_path& d, bool verbose = true)
    {
      try
      {
        if (dir_exists (d)) // May throw (e.g., EACCES).
          return;
      }
      catch (const system_error& e)
      {
        fail << "invalid installation directory " << d << ": " << e.what ();
      }

      // While install -d will create all the intermediate components between
      // base and dir, we do it explicitly, one at a time. This way the output
      // is symmetrical to uninstall() below.
      //
      if (d != base.dir)
      {
        dir_path pd (d.directory ());

        if (pd != base.dir)
          install_d (base, pd, verbose);
      }

      cstrings args;

      string reld (
        cast<string> ((*global_scope)["build.host.class"]) == "windows"
        ? msys_path (d)
        : relative (d).string ());

      if (base.sudo != nullptr)
        args.push_back (base.sudo->c_str ());

      args.push_back (base.cmd->string ().c_str ());
      args.push_back ("-d");

      if (base.options != nullptr)
        append_options (args, *base.options);

      args.push_back ("-m");
      args.push_back (base.dir_mode->c_str ());
      args.push_back (reld.c_str ());
      args.push_back (nullptr);

      try
      {
        process_path pp (process::path_search (args[0]));

        if (verb >= 2)
          print_process (args);
        else if (verb && verbose)
          text << "install " << d;

        process pr (pp, args.data ());

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

    // install <file> <dir>/
    // install <file> <file>
    //
    // If verbose is false, then only print the command at verbosity level 2
    // or higher.
    //
    static void
    install_f (const install_dir& base,
               const path& name,
               file& t,
               bool verbose)
    {
      path relf (relative (t.path ()));

      string reld (
        cast<string> ((*global_scope)["build.host.class"]) == "windows"
        ? msys_path (base.dir)
        : relative (base.dir).string ());

      if (!name.empty ())
      {
        reld += path::traits::directory_separator;
        reld += name.string ();
      }

      cstrings args;

      if (base.sudo != nullptr)
        args.push_back (base.sudo->c_str ());

      args.push_back (base.cmd->string ().c_str ());

      if (base.options != nullptr)
        append_options (args, *base.options);

      args.push_back ("-m");
      args.push_back (base.mode->c_str ());
      args.push_back (relf.string ().c_str ());
      args.push_back (reld.c_str ());
      args.push_back (nullptr);

      try
      {
        process_path pp (process::path_search (args[0]));

        if (verb >= 2)
          print_process (args);
        else if (verb && verbose)
          text << "install " << t;

        process pr (pp, args.data ());

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

    void file_rule::
    install_l (const install_dir& base,
               const path& target,
               const path& link,
               bool verbose)
    {
      path rell (relative (base.dir));
      rell /= link;

      // We can create a symlink directly without calling ln. This, however,
      // won't work if we have sudo. Also, we would have to deal with existing
      // destinations (ln's -f takes care of that). So we are just going to
      // always use ln.
      //
      const char* args_a[] = {
        base.sudo != nullptr ? base.sudo->c_str () : nullptr,
        "ln",
        "-sf",
        target.string ().c_str (),
        rell.string ().c_str (),
        nullptr};

      const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

      try
      {
        process_path pp (process::path_search (args[0]));

        if (verb >= 2)
          print_process (args);
        else if (verb && verbose)
          text << "install " << rell << " -> " << target;

        process pr (pp, args);

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

    target_state file_rule::
    perform_install (action a, target& xt) const
    {
      file& t (static_cast<file&> (xt));
      assert (!t.path ().empty ()); // Should have been assigned by update.

      auto install_target = [this] (file& t, const path& p, bool verbose)
      {
        bool n (!p.to_directory ());
        dir_path d (n ? p.directory () : path_cast<dir_path> (p));

        // Resolve target directory.
        //
        install_dirs ids (resolve (t, d));

        // Handle install.subdirs if one was specified.
        //
        if (auto l = t["install.subdirs"])
        {
          if (cast<bool> (l))
            resolve_subdir (ids, t, t.base_scope (), l);
        }

        // Create leading directories. Note that we are using the leading
        // directory (if there is one) for the creation information (mode,
        // sudo, etc).
        //
        for (auto i (ids.begin ()), j (i); i != ids.end (); j = i++)
          install_d (*j, i->dir, verbose); // install -d

        install_dir& id (ids.back ());

        // Override mode if one was specified.
        //
        if (auto l = t["install.mode"])
          id.mode = &cast<string> (l);

        // Install the target and extras.
        //
        install_f (id, n ? p.leaf () : path (), t, verbose);
        install_extra (t, id);
      };

      // First handle installable prerequisites.
      //
      target_state r (execute_prerequisites (a, t));

      // Then installable ad hoc group members, if any.
      //
      for (target* m (t.member); m != nullptr; m = m->member)
      {
        if (const path* p = lookup_install<path> (*m, "install"))
          install_target (static_cast<file&> (*m), *p, false);
      }

      // Finally install the target itself (since we got here we know the
      // install variable is there).
      //
      install_target (t, cast<path> (t["install"]), true);

      return (r |= target_state::changed);
    }

    // uninstall -d <dir>
    //
    // We try remove all the directories between base and dir but not base
    // itself unless base == dir. Return false if nothing has been removed
    // (i.e., the directories do not exist or are not empty).
    //
    // If verbose is false, then only print the command at verbosity level 2
    // or higher.
    //
    static bool
    uninstall_d (const install_dir& base, const dir_path& d, bool verbose)
    {
      // Figure out if we should try to remove this directory. Note that if
      // it doesn't exist, then we may still need to remove outer ones.
      //
      bool r (false);
      try
      {
        if ((r = dir_exists (d))) // May throw (e.g., EACCES).
        {
          if (!dir_empty (d)) // May also throw.
            return false; // Won't be able to remove any outer directories.
        }
      }
      catch (const system_error& e)
      {
        fail << "invalid installation directory " << d << ": " << e.what ();
      }

      if (r)
      {
        dir_path reld (relative (d));

        // Normally when we need to remove a file or directory we do it
        // directly without calling rm/rmdir. This however, won't work if we
        // have sudo. So we are going to do it both ways.
        //
        // While there is no sudo on Windows, deleting things that are being
        // used can get complicated. So we will always use rm/rmdir there.
        //
#ifndef _WIN32
        if (base.sudo == nullptr)
        {
          if (verb >= 2)
            text << "rmdir " << reld;
          else if (verb && verbose)
            text << "uninstall " << reld;

          try
          {
            try_rmdir (d);
          }
          catch (const system_error& e)
          {
            fail << "unable to remove directory " << d << ": " << e.what ();
          }
        }
        else
#endif
        {
          const char* args_a[] = {
            base.sudo != nullptr ? base.sudo->c_str () : nullptr,
            "rmdir",
            reld.string ().c_str (),
            nullptr};

          const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

          try
          {
            process_path pp (process::path_search (args[0]));

            if (verb >= 2)
              print_process (args);
            else if (verb && verbose)
              text << "uninstall " << reld;

            process pr (pp, args);

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
      }

      // If we have more empty directories between base and dir, then try
      // to clean them up as well.
      //
      if (d != base.dir)
      {
        dir_path pd (d.directory ());

        if (pd != base.dir)
          r = uninstall_d (base, pd, verbose) || r;
      }

      return r;
    }

    bool file_rule::
    uninstall_f (const install_dir& base,
                 file* t,
                 const path& name,
                 bool verbose)
    {
      assert (t != nullptr || !name.empty ());
      path f (base.dir / (name.empty () ? t->path ().leaf () : name));

      try
      {
        // Note: don't follow symlinks so if the target is a dangling symlinks
        // we will proceed to removing it.
        //
        if (!file_exists (f, false)) // May throw (e.g., EACCES).
          return false;
      }
      catch (const system_error& e)
      {
        fail << "invalid installation path " << f << ": " << e.what ();
      }

      path relf (relative (f));

      if (verb == 1 && verbose)
      {
        if (t != nullptr)
          text << "uninstall " << *t;
        else
          text << "uninstall " << relf;
      }

      // The same story as with uninstall -d.
      //
#ifndef _WIN32
      if (base.sudo == nullptr)
      {
        if (verb >= 2)
          text << "rm " << relf;

        try
        {
          try_rmfile (f);
        }
        catch (const system_error& e)
        {
          fail << "unable to remove file " << f << ": " << e.what ();
        }
      }
      else
#endif
      {
        const char* args_a[] = {
          base.sudo != nullptr ? base.sudo->c_str () : nullptr,
          "rm",
          "-f",
          relf.string ().c_str (),
          nullptr};

        const char** args (&args_a[base.sudo == nullptr ? 1 : 0]);

        try
        {
          process_path pp (process::path_search (args[0]));

          if (verb >= 2)
            print_process (args);

          process pr (pp, args);

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

      return true;
    }

    target_state file_rule::
    perform_uninstall (action a, target& xt) const
    {
      file& t (static_cast<file&> (xt));
      assert (!t.path ().empty ()); // Should have been assigned by update.

      auto uninstall_target = [this] (file& t, const path& p, bool verbose)
        -> target_state
      {
        bool n (!p.to_directory ());
        dir_path d (n ? p.directory () : path_cast<dir_path> (p));

        // Resolve target directory.
        //
        install_dirs ids (resolve (t, d));

        // Handle install.subdirs if one was specified.
        //
        if (auto l = t["install.subdirs"])
        {
          if (cast<bool> (l))
            resolve_subdir (ids, t, t.base_scope (), l);
        }

        // Remove extras and the target itself.
        //
        const install_dir& id (ids.back ());

        target_state r (uninstall_extra (t, id)
                        ? target_state::changed
                        : target_state::unchanged);

        if (uninstall_f (id, &t, n ? p.leaf () : path (), verbose))
          r |= target_state::changed;

        // Clean up empty leading directories (in reverse).
        //
        // Note that we are using the leading directory (if there is one)
        // for the clean up information (sudo, etc).
        //
        for (auto i (ids.rbegin ()), j (i), e (ids.rend ()); i != e; j = ++i)
        {
          if (install::uninstall_d (++j != e ? *j : *i, i->dir, verbose))
            r |= target_state::changed;
        }

        return r;
      };

      // Reverse order of installation: first the target itself (since we got
      // here we know the install variable is there).
      //
      target_state r (uninstall_target (t, cast<path> (t["install"]), true));

      // Then installable ad hoc group members, if any. To be anally precise
      // we would have to do it in reverse, but that's not easy (it's a
      // single-linked list).
      //
      for (target* m (t.member); m != nullptr; m = m->member)
      {
        if (const path* p = lookup_install<path> (*m, "install"))
          r |= uninstall_target (static_cast<file&> (*m),
                                 *p,
                                 r != target_state::changed);
      }

      // Finally handle installable prerequisites.
      //
      r |= reverse_execute_prerequisites (a, t);

      return r;
    }
  }
}
