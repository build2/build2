// file      : libbuild2/config/operation.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/operation.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/buildspec.hxx>  // opspec
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/module.hxx>
#include <libbuild2/config/utility.hxx> // save_*

using namespace std;
using namespace butl;

namespace build2
{
  namespace config
  {
    // configure
    //
    static void
    save_src_root (const scope& rs)
    {
      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      path f (out_root / rs.root_extra->src_root_file);

      if (verb >= 2)
        text << "cat >" << f;

      try
      {
        ofdstream ofs (f);

        ofs << "# Created automatically by the config module." << endl
            << "#" << endl
            << "src_root = ";
        to_stream (ofs, name (src_root), quote_mode::normal, '@');
        ofs << endl;

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << f << ": " << e;
      }
    }

    static void
    save_out_root (const scope& rs)
    {
      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      path f (src_root / rs.root_extra->out_root_file);

      if (verb >= 2)
        text << "cat >" << f;
      else if (verb)
        print_diag ("save", f);

      try
      {
        ofdstream ofs (f);

        ofs << "# Created automatically by the config module." << endl
            << "#" << endl
            << "out_root = ";
        to_stream (ofs, name (out_root), quote_mode::normal, '@');
        ofs << endl;

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << f << ": " << e;
      }
    }

    // Return (first) whether an unused/inherited variable should be saved
    // according to the config.config.persist value and (second) whether the
    // user should be warned about it.
    //
    static pair<bool, bool>
    save_config_variable (const variable& var,
                          const vector<pair<string, string>>* persist,
                          bool inherited,
                          bool unused)
    {
      assert (inherited || unused);

      if (persist != nullptr)
      {
        for (const pair<string, string>& pc: reverse_iterate (*persist))
        {
          if (!path_match (var.name, pc.first))
            continue;

          const string& c (pc.second);

          size_t p;
          if      (c.compare (0, (p = 7),  "unused=") == 0)
          {
            if (!unused || inherited)
              continue;
          }
          else if (c.compare (0, (p = 10), "inherited=") == 0)
          {
            // Applies to both used an unused.
            //
            if (!inherited)
              continue;
          }
          else if (c.compare (0, (p = 15), "inherited-used=") == 0)
          {
            if (!inherited || unused)
              continue;
          }
          else if (c.compare (0, (p = 17), "inherited-unused=") == 0)
          {
            if (!inherited || !unused)
              continue;
          }
          else
            fail << "invalid config.config.persist condition '" << c << "'";

          bool r;
          if      (c.compare (p, 4 , "save") == 0) r = true;
          else if (c.compare (p, 4 , "drop") == 0) r = false;
          else fail << "invalid config.config.persist action '" << c << "'"
                    << endf;

          bool w (false);
          if ((p += 4) != c.size ())
          {
            if (c.compare (p, string::npos, "+warn") == 0) w = true;
            else fail << "invalid config.config.persist action '" << c << "'";
          }

          return make_pair (r, w);
        }
      }

      // Defaults.
      //
      if (!inherited)
        return make_pair (false, true);  // unused:           drop  warn
      else if (unused)
        return make_pair (true,  true);  // inherited-unused: save  warn
      else
        return make_pair (false, false); // inherited-used:   drop !warn
    }

    // If inherit is false, then don't rely on inheritance from outer scopes.
    //
    // @@ We are modifying the module (marking additional variables as saved)
    //    and this function can be called from a buildfile (probably only
    //    during serial execution but still).
    //
    //    We could also be configuring multiple projects (including from
    //    pkg_configure() in bpkg) but feels like we should be ok since we
    //    only modify this project's root scope data which should not affect
    //    any other project.
    //
    //    See also save_environment() for a similar issue.
    //
    void
    save_config (const scope& rs,
                 ostream& os, const path_name& on,
                 bool inherit,
                 const module& mod,
                 const project_set& projects)
    {
      context& ctx (rs.ctx);

      names storage;

      auto info_value = [&storage] (diag_record& dr, const value& v) mutable
      {
        dr << info << "variable value: ";

        if (v)
        {
          storage.clear ();
          dr << "'" << reverse (v, storage, true /* reduce */) << "'";
        }
        else
          dr << "[null]";
      };

      try
      {
        os << "# Created automatically by the config module, but feel " <<
          "free to edit." << endl
           << "#" << endl;

        os << "config.version = " << module::version << endl;

        if (inherit)
        {
          if (const dir_path* a = *rs.root_extra->amalgamation)
          {
            os << endl
               << "# Base configuration inherited from " << *a << endl
               << "#" << endl;
          }
        }

        // Mark the unused config.* variables defined on our root scope as
        // saved according to config.config.persist potentially warning if the
        // variable would otherwise be dropped.
        //
        // Note: go straight for the public variable pool.
        //
        auto& vp (ctx.var_pool);

        for (auto p (rs.vars.lookup_namespace ("config"));
             p.first != p.second;
             ++p.first)
        {
          const variable* var (&p.first->first.get ());

          // Annoyingly, this can be one of the overrides (__override,
          // __prefix, etc).
          //
          if (size_t n = var->override ())
            var = vp.find (string (var->name, 0, n));

          const string& name (var->name);

          // Skip special variables.
          //
          if (name == "config.booted"                   ||
              name == "config.loaded"                   ||
              name == "config.configured"               ||
              name.compare (0, 14, "config.config.") == 0)
            continue;

          if (mod.find_variable (*var)) // Saved or unsaved.
            continue;

          // Skip config.**.develop variables (see parser::parse_config() for
          // details).
          //
          // In a sense, this variable is always "available" but if the
          // package does not distinguish between development and consumption,
          // then specifying config.*.develop=true should be noop.
          //
          {
            size_t p (name.rfind ('.'));
            if (p != 6 && name.compare (p + 1, string::npos, "develop") == 0)
              continue;
          }

          // A common reason behind an unused config.import.* value is an
          // unused dependency. That is, there is depends in manifest but no
          // import in buildfile (or import could be conditional in which case
          // depends should also be conditional). So let's suggest this
          // possibility. Note that the project name may have been sanitized
          // to a variable name. Oh, well, better than nothing.
          //
          auto info_import = [] (diag_record& dr, const string& var)
          {
            if (var.compare (0, 14, "config.import.") == 0)
            {
              size_t p (var.find ('.', 14));

              dr << info << "potentially unused dependency on "
                 << string (var, 14, p == string::npos ? p : p - 14);
            }
          };

          const value& v (p.first->second);

          pair<bool, bool> r (save_config_variable (*var,
                                                    mod.persist,
                                                    false /* inherited */,
                                                    true  /* unused */));
          if (r.first) // save
          {
            const_cast<module&> (mod).save_variable (*var, 0);

            if (r.second) // warn
            {
              // Consistently with save_config() below we don't warn about an
              // overriden variable.
              //
              if (var->overrides != nullptr)
              {
                lookup l {v, *var, rs.vars};
                pair<lookup, size_t> org {l, 1 /* depth */};
                pair<lookup, size_t> ovr (rs.lookup_override (*var, org));

                if (org.first != ovr.first)
                  continue;
              }

              diag_record dr;
              dr << warn (on) << "saving no longer used variable " << *var;
              info_import (dr, var->name);
              if (verb >= 2)
                info_value (dr, v);
            }
          }
          else // drop
          {
            if (r.second) // warn
            {
              diag_record dr;
              dr << warn (on) << "dropping no longer used variable " << *var;
              info_import (dr, var->name);
              info_value (dr, v);
            }
          }
        }

        // Save config variables.
        //
        for (auto p: mod.saved_modules.order)
        {
          const string& sname (p.second->first);
          const saved_variables& svars (p.second->second);

          // Separate modules with a blank line.
          //
          auto first = [v = true] () mutable
          {
            if (v)
            {
              v = false;
              return "\n";
            }
            return "";
          };

          for (const saved_variable& sv: svars)
          {
            if (!sv.flags) // unsaved
              continue;

            const variable& var (sv.var);
            uint64_t flags (*sv.flags);

            pair<lookup, size_t> org (rs.lookup_original (var));
            pair<lookup, size_t> ovr (var.overrides == nullptr
                                      ? org
                                      : rs.lookup_override (var, org));
            const lookup& l (ovr.first);

            // We definitely write values that are set on our root scope or
            // are global overrides. Anything in-between is presumably
            // inherited. We might also not have any value at all (see
            // unconfigured()).
            //
            // Note that we must check for null() before attempting any
            // further tests.
            //
            if (!l.defined () ||
                (l->null     ? flags & save_null_omitted  :
                 l->empty () ? flags & save_empty_omitted :
                 (flags & save_false_omitted) != 0 && !cast<bool> (*l)))
              continue;

            // Handle inherited from outer scope values.
            //
            // Note that we skip this entire logic if inherit is false since
            // we save the inherited values regardless of whether they are
            // used or not.
            //
            const value* base (nullptr);
            if (inherit)
            {
              // Return true if the specified value can be inherited from.
              //
              auto find_inherited = [&on, &projects,
                                     &info_value,
                                     &sname, &rs, &var] (const lookup& org,
                                                         const lookup& ovr)
              {
                const lookup& l (ovr);

                // This is presumably an inherited value. But it could also be
                // some left-over garbage. For example, an amalgamation could
                // have used a module but then dropped it while its config
                // values are still lingering in config.build. They are
                // probably still valid and we should probably continue using
                // them but we definitely want to move them to our
                // config.build since they will be dropped from the
                // amalgamation's config.build on the next reconfigure. Let's
                // also warn the user just in case, unless there is no module
                // and thus we couldn't really check (the latter could happen
                // when calling $config.save() during other meta-operations,
                // though it passes false for inherit).
                //
                // There is also another case that falls under this now that
                // overrides are by default amalgamation-wide rather than just
                // "project and subprojects": we may be (re-)configuring a
                // subproject but the override is now set on the outer
                // project's root.
                //
                bool found (false), checked (true);
                const scope* r (&rs);
                while ((r = r->parent_scope ()->root_scope ()) != nullptr)
                {
                  if (l.belongs (*r))
                  {
                    // Find the config module (might not be there).
                    //
                    if (auto* m = r->find_module<const module> (module::name))
                    {
                      // Find the corresponding saved module.
                      //
                      auto i (m->saved_modules.find (sname));

                      if (i != m->saved_modules.end ())
                      {
                        // Find the variable.
                        //
                        const saved_variables& sv (i->second);
                        found = sv.find (var) != sv.end ();

                        // If not marked as saved, check whether overriden via
                        // config.config.persist.
                        //
                        if (!found && m->persist != nullptr)
                        {
                          found = save_config_variable (
                            var,
                            m->persist,
                            false /* inherited */,
                            true  /* unused */).first;
                        }

                        // Handle that other case: if this is an override but
                        // the outer project itself is not being configured,
                        // then we need to save this override.
                        //
                        // One problem with using the already configured
                        // project set is that the outer project may be
                        // configured only after us in which case both
                        // projects will save the value. But perhaps this is a
                        // feature, not a bug since this is how project-local
                        // (%) override behaves.
                        //
                        if (found &&
                            org != ovr &&
                            projects.find (r) == projects.end ())
                          found = false;
                      }
                    }
                    else
                      checked = false;

                    break;
                  }
                }

                if (found)
                  return true;

                // If this value is not defined in a project's root scope,
                // then something is broken.
                //
                if (r == nullptr)
                  fail (on) << "inherited variable " << var << " value is not "
                            << "from a root scope";

                // If none of the outer project's configurations use this
                // value, then we warn (unless we couldn't check) and save as
                // our own. One special case where we don't want to warn the
                // user is if the variable is overriden.
                //
                if (checked && org == ovr)
                {
                  diag_record dr;
                  dr << warn (on) << "saving previously inherited variable "
                     << var;

                  dr << info << "because project " << *r << " no longer uses "
                     << "it in its configuration";

                  if (verb >= 2)
                    info_value (dr, *l);
                }

                return false;
              };

              // Inherit as-is.
              //
              if (!l.belongs (rs) &&
                  !l.belongs (ctx.global_scope) &&
                  find_inherited (org.first, ovr.first))
                continue;
              else if (flags & save_base)
              {
                // See if we can base our value on inherited.
                //
                if (const scope* ors = rs.parent_scope ()->root_scope ())
                {
                  pair<lookup, size_t> org (ors->lookup_original (var));
                  pair<lookup, size_t> ovr (var.overrides == nullptr
                                            ? org
                                            : ors->lookup_override (var, org));
                  const lookup& l (ovr.first);

                  // We cannot base anything on an empty value.
                  //
                  if (l && !l->empty ())
                  {
                    // @@ It's not clear we want the checks/diagnostics in
                    //    this case.
                    //
                    if (find_inherited (org.first, ovr.first))
                      base = l.value;
                  }
                }
              }
            }

            const string& n (var.name);
            const value& v (*l);

            // We will only write config.*.configured if it is false (true is
            // implied by its absence). We will also ignore false values if
            // there is any other value for this module (see unconfigured()).
            //
            if (n.size () > 11 &&
                n.compare (n.size () - 11, 11, ".configured") == 0)
            {
              if (cast<bool> (v) || svars.size () != 1)
                continue;
            }

            // Handle the save_default_commented flag.
            //
            if (org.first.defined () && org.first->extra == 1 && // Default.
                org.first == ovr.first &&                        // No override.
                (flags & save_default_commented) != 0)
            {
              os << first () << '#' << n << " =" << endl;
              continue;
            }

            if (v.null)
            {
              os << first () << n << " = [null]" << endl;
              continue;
            }

            storage.clear ();
            pair<names_view, const char*> p (
              sv.save != nullptr
              ? sv.save (v, base, storage)
              : make_pair (reverse (v, storage, true /* reduce */), "="));

            // Might becomes empty after a custom save function had at it.
            //
            if (p.first.empty () && (flags & save_empty_omitted))
              continue;

            os << first () << n << ' ' << p.second;

            if (!p.first.empty ())
            {
              os << ' ';
              to_stream (os, p.first, quote_mode::normal, '@');
            }

            os << endl;
          }
        }
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << on << ": " << e;
      }
    }

    static void
    save_config (const scope& rs,
                 const path& f,
                 bool inherit,
                 const module& mod,
                 const project_set& projects)
    {
      path_name fn (f);

      if (f.string () == "-")
        fn.name = "<stdout>";

      if (verb >= 2)
        text << "cat >" << fn;
      else if (verb)
        print_diag ("save", fn);

      try
      {
        ofdstream ofs;
        save_config (
          rs, open_file_or_stdout (fn, ofs), fn, inherit, mod, projects);
        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << fn << ": " << e;
      }
    }

    // Update config.config.environment value for a hermetic configuration.
    //
    // @@ We are modifying the module. See also save_config() for a similar
    //    issue.
    //
    static void
    save_environment (scope& rs, module& mod)
    {
      // Here we have two parts: (1) get the list of environment variables we
      // need to save and (2) save their values in config.config.environment.
      //
      // The saved_environment list should by now contain all the project-
      // specific environment variables. To that we add builtin defaults and
      // then filter the result against config.config.hermetic.environment
      // inclusions/exclusions.
      //
      auto& vars (mod.saved_environment);

      vars.insert ("PATH");

#if   defined(_WIN32)
#elif defined(__APPLE__)
      vars.insert ("DYLD_LIBRARY_PATH");
#else // Linux, FreeBSD, NetBSD, OpenBSD
      vars.insert ("LD_LIBRARY_PATH");
#endif

      for (const pair<string, optional<bool>>& p:
             cast_empty<hermetic_environment> (
               rs["config.config.hermetic.environment"]))
      {
        if (!p.second || *p.second)
          vars.insert (p.first);
        else
          vars.erase (p.first);
      }

      // Get the values.
      //
      strings vals;
      {
        // Set the project environment before querying the values. Note that
        // the logic in init() makes sure that this is all we need to do to
        // handle reload (in which case we should still be using
        // config.config.environment from amalgamation, if any).
        //
        auto_project_env penv (rs);

        for (const string& var: vars)
        {
          if (optional<string> val = getenv (var))
          {
            vals.push_back (var + '=' + *val);
          }
          else
            vals.push_back (var); // Unset.
        }
      }

      // Note: go straight for the public variable pool.
      //
      value& v (rs.assign (*rs.ctx.var_pool.find ("config.config.environment")));

      // Note that setting new config.config.environment value invalidates the
      // project's environment (scope::root_extra::environment) which could be
      // queried in the post-configuration hook. We could re-initialize it but
      // the c.c.e value from amalgamation could be referenced by subprojects.
      // So instead it seems easier to just save the old value in the module.
      //
      if (v)
        mod.old_environment = move (v.as<strings> ());

      v = move (vals);
    }

    static void
    configure_project (action a,
                       const scope& rs,
                       const variable* c_s, // config.config.save
                       const module& mod,
                       project_set& projects)
    {
      tracer trace ("configure_project");

      context& ctx (rs.ctx);

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      if (!projects.insert (&rs).second)
      {
        l5 ([&]{trace << "skipping already configured " << out_root;});
        return;
      }

      // Make sure the directories exist.
      //
      if (out_root != src_root)
      {
        mkdir_p (out_root / rs.root_extra->build_dir, 1);
        mkdir (out_root / rs.root_extra->bootstrap_dir, 2);
      }

      // We distinguish between a complete configure and operation-specific.
      //
      if (a.operation () == default_id)
      {
        l5 ([&]{trace << "completely configuring " << out_root;});

        // Save the environment if this configuration is hermetic (see init()
        // for the other half of this logic).
        //
        if (cast_false<bool> (rs["config.config.hermetic"]))
          save_environment (const_cast<scope&> (rs), const_cast<module&> (mod));

        // Save src-root.build unless out_root is the same as src.
        //
        if (c_s == nullptr && out_root != src_root)
          save_src_root (rs);

        // Save config.build unless an alternative is specified with
        // config.config.save. Similar to config.config.load we will only save
        // to that file if it is specified on our root scope or as a global
        // override (the latter is a bit iffy but let's allow it, for example,
        // to dump everything to stdout). Note that to save a subproject's
        // config we will have to use a scope-specific override (since the
        // default will apply to the amalgamation):
        //
        // b configure: subproj/ subproj/config.config.save=.../config.build
        //
        // Could be confusing but then normally it will be the amalgamation
        // whose configuration we want to export.
        //
        // Note also that if config.config.save is specified we do not rewrite
        // config.build files (say, of subprojects) as well as src-root.build
        // above. Failed that, if we are running in a disfigured project, we
        // may end up leaving it in partially configured state.
        //
        if (c_s == nullptr)
          save_config (rs, config_file (rs), true /* inherit */, mod, projects);
        else
        {
          lookup l (rs[*c_s]);
          if (l && (l.belongs (rs) || l.belongs (ctx.global_scope)))
          {
            const path& f (cast<path> (l));

            if (f.empty ())
              fail << "empty path in " << *c_s;

            // While writing the complete configuration seems like a natural
            // default, there might be a desire to take inheritance into
            // account (if, say, we are exporting at multiple levels). One can
            // of course just copy the relevant config.build files, but we may
            // still want to support this mode somehow in the future (it seems
            // like an override of config.config.persist should do the trick).
            //
            save_config (rs, f, false /* inherit */, mod, projects);
          }
        }
      }
      else
      {
        fail << "operation-specific configuration not yet supported";
      }

      if (c_s == nullptr)
      {
        for (auto hook: mod.configure_post_)
          hook (a, rs);
      }

      // Configure subprojects that have been loaded.
      //
      if (const subprojects* ps = *rs.root_extra->subprojects)
      {
        for (auto p: *ps)
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          const scope& nrs (ctx.scopes.find_out (out_nroot));

          // Skip this subproject if it is not loaded or doesn't use the
          // config module.
          //
          if (nrs.out_path () == out_nroot)
          {
            if (const module* m = nrs.find_module<module> (module::name))
            {
              configure_project (a, nrs, c_s, *m, projects);
            }
          }

        }
      }
    }

    static void
    configure_forward (const scope& rs, project_set& projects)
    {
      tracer trace ("configure_forward");

      context& ctx (rs.ctx);

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      if (!projects.insert (&rs).second)
      {
        l5 ([&]{trace << "skipping already configured " << src_root;});
        return;
      }

      mkdir (src_root / rs.root_extra->bootstrap_dir, 2); // Make sure exists.
      save_out_root (rs);

      // Configure subprojects. Since we don't load buildfiles if configuring
      // a forward, we do it for all known subprojects.
      //
      if (const subprojects* ps = *rs.root_extra->subprojects)
      {
        for (auto p: *ps)
        {
          dir_path out_nroot (out_root / p.second);
          const scope& nrs (ctx.scopes.find_out (out_nroot));
          assert (nrs.out_path () == out_nroot);

          configure_forward (nrs, projects);
        }
      }
    }

    operation_id (*pre) (const values&, meta_operation_id, const location&);

    static operation_id
    configure_operation_pre (context&, const values&, operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means configure everything.
      //
      // Note: see pkg_configure() in bpkg if changing anything here.
      //
      return o;
    }

    // The (vague) idea is that in the future we may turn this into to some
    // sort of key-value sequence (similar to the config initializer idea),
    // for example:
    //
    // configure(out/@src/, forward foo bar@123)
    //
    // Though using commas instead spaces and '=' instead of '@' would have
    // been nicer.
    //
    static bool
    forward (const values& params,
             const char* mo = nullptr,
             const location& l = location ())
    {
      if (params.size () == 1)
      {
        const names& ns (cast<names> (params[0]));

        if (ns.size () == 1 && ns[0].simple () && ns[0].value == "forward")
          return true;
        else if (!ns.empty ())
          fail (l) << "unexpected parameter '" << ns << "' for "
                   << "meta-operation " << mo;
      }
      else if (!params.empty ())
        fail (l) << "unexpected parameters for meta-operation " << mo;

      return false;
    }

    static void
    configure_pre (context&, const values& params, const location& l)
    {
      // Note: see pkg_configure() in bpkg if changing anything here.
      //
      forward (params, "configure", l); // Validate.
    }

    static void
    configure_load (const values& params,
                    scope& rs,
                    const path& buildfile,
                    const dir_path& out_base,
                    const dir_path& src_base,
                    const location& l)
    {
      if (forward (params))
      {
        // We don't need to load the buildfiles in order to configure
        // forwarding but in order to configure subprojects we have to
        // bootstrap them (similar to disfigure).
        //
        create_bootstrap_inner (rs);

        if (rs.out_eq_src ())
          fail (l) << "forwarding to source directory " << rs.src_path ();
      }
      else
        // Normal load.
        //
        perform_load (params, rs, buildfile, out_base, src_base, l);
    }

    static void
    configure_search (const values& params,
                      const scope& rs,
                      const scope& bs,
                      const path& bf,
                      const target_key& tk,
                      const location& l,
                      action_targets& ts)
    {
      if (forward (params))
      {
        // For forwarding we only collect the projects (again, similar to
        // disfigure).
        //
        ts.push_back (&rs);
      }
      else
        perform_search (params, rs, bs, bf, tk, l, ts); // Normal search.
    }

    static void
    configure_match (const values&, action, action_targets&, uint16_t, bool)
    {
      // Don't match anything -- see execute ().
    }

    static void
    configure_execute (const values& params,
                       action a,
                       action_targets& ts,
                       uint16_t,
                       bool)
    {
      bool fwd (forward (params));

      context& ctx (fwd ? ts[0].as<scope> ().ctx : ts[0].as<target> ().ctx);

      // Note: go straight for the public variable pool.
      //
      const variable* c_s (ctx.var_pool.find ("config.config.save"));

      if (c_s->overrides == nullptr)
        c_s = nullptr;
      else if (fwd)
        fail << "config.config.save specified for forward configuration";

      project_set projects;

      for (const action_target& at: ts)
      {
        if (fwd)
        {
          // Forward configuration.
          //
          const scope& rs (at.as<scope> ());
          configure_forward (rs, projects);
        }
        else
        {
          // Normal configuration.
          //
          // Match rules to configure every operation supported by each
          // project. Note that we are not calling operation_pre/post()
          // callbacks here since the meta operation is configure and we know
          // what we are doing.
          //
          // Note that we cannot do this in parallel. We cannot parallelize
          // the outer loop because we should match for a single action at a
          // time. And we cannot swap the loops because the list of operations
          // is target-specific. However, inside match(), things can proceed
          // in parallel.
          //
          const target& t (at.as<target> ());
          const scope* rs (t.base_scope ().root_scope ());

          if (rs == nullptr)
            fail << "out of project target " << t;

          const operations& ops (rs->root_extra->operations);

          for (operation_id id (default_id + 1); // Skip default_id.
               id < ops.size ();
               ++id)
          {
            if (const operation_info* oif = ops[id])
            {
              // Skip aliases (e.g., update-for-install).
              //
              if (oif->id != id)
                continue;

              ctx.current_operation (*oif);

              if (oif->operation_pre != nullptr)
                oif->operation_pre (ctx, {}, true /* inner */, location ());

              phase_lock pl (ctx, run_phase::match);
              match_sync (action (configure_id, id), t);

              if (oif->operation_post != nullptr)
                oif->operation_post (ctx, {}, true /* inner */);
            }
          }

          configure_project (a,
                             *rs,
                             c_s,
                             *rs->find_module<module> (module::name),
                             projects);
        }
      }
    }

    // NOTE: see pkg_configure() in bpkg if changing anything here.
    //
    const meta_operation_info mo_configure {
      configure_id,
      "configure",
      "configure",
      "configuring",
      "configured",
      "is configured",
      true,           // bootstrap_outer
      &configure_pre, // meta-operation pre
      &configure_operation_pre,
      &configure_load,   // normal load unless configuring forward
      &configure_search, // normal search unless configuring forward
      &configure_match,
      &configure_execute,
      nullptr, // operation post
      nullptr, // meta-operation post
      nullptr  // include
    };

    // disfigure
    //

    static bool
    disfigure_project (action a, const scope& rs, project_set& projects)
    {
      tracer trace ("disfigure_project");

      context& ctx (rs.ctx);

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      if (!projects.insert (&rs).second)
      {
        l5 ([&]{trace << "skipping already disfigured " << out_root;});
        return false;
      }

      bool r (false); // Keep track of whether we actually did anything.

      // Disfigure subprojects. Since we don't load buildfiles during
      // disfigure, we do it for all known subprojects.
      //
      if (const subprojects* ps = *rs.root_extra->subprojects)
      {
        for (auto p: *ps)
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          const scope& nrs (ctx.scopes.find_out (out_nroot));
          assert (nrs.out_path () == out_nroot); // See disfigure_load().

          r = disfigure_project (a, nrs, projects) || r;

          // We use mkdir_p() to create the out_root of a subproject
          // which means there could be empty parent directories left
          // behind. Clean them up.
          //
          if (!pd.simple () && out_root != src_root)
          {
            for (dir_path d (pd.directory ());
                 !d.empty ();
                 d = d.directory ())
            {
              rmdir_status s (rmdir (ctx, out_root / d, 2));

              if (s == rmdir_status::not_empty)
                break; // No use trying do remove parent ones.

              r = (s == rmdir_status::success) || r;
            }
          }
        }
      }

      if (const module* m = rs.find_module<module> (module::name))
      {
        for (auto hook: m->disfigure_pre_)
          r = hook (a, rs) || r;
      }

      // We distinguish between a complete disfigure and operation-
      // specific.
      //
      if (a.operation () == default_id)
      {
        l5 ([&]{trace << "completely disfiguring " << out_root;});

        r = rmfile (ctx, config_file (rs)) || r;

        if (out_root != src_root)
        {
          r = rmfile (ctx, out_root / rs.root_extra->src_root_file, 2) || r;

          // Clean up the directories.
          //
          // Note: try to remove the root/ hooks directory if it is empty.
          //
          r = rmdir (ctx, out_root / rs.root_extra->root_dir,      2) || r;
          r = rmdir (ctx, out_root / rs.root_extra->bootstrap_dir, 2) || r;
          r = rmdir (ctx, out_root / rs.root_extra->build_dir,     2) || r;

          switch (rmdir (ctx, out_root))
          {
          case rmdir_status::not_empty:
            {
              // We used to issue a warning but it is actually a valid usecase
              // to leave the build output around in case, for example, of a
              // reconfigure.
              //
              if (verb)
                info << "directory " << out_root << " is "
                     << (out_root == work
                         ? "current working directory"
                         : "not empty") << ", not removing";
              break;
            }
          case rmdir_status::success:
            r = true;
          default:
            break;
          }
        }
      }
      else
      {
        fail << "operation-specific disfiguration not yet supported";
      }

      return r;
    }

    static bool
    disfigure_forward (const scope& rs, project_set& projects)
    {
      // Pretty similar logic to disfigure_project().
      //
      tracer trace ("disfigure_forward");

      context& ctx (rs.ctx);

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      if (!projects.insert (&rs).second)
      {
        l5 ([&]{trace << "skipping already disfigured " << src_root;});
        return false;
      }

      bool r (false);

      if (const subprojects* ps = *rs.root_extra->subprojects)
      {
        for (auto p: *ps)
        {
          dir_path out_nroot (out_root / p.second);
          const scope& nrs (ctx.scopes.find_out (out_nroot));
          assert (nrs.out_path () == out_nroot);

          r = disfigure_forward (nrs, projects) || r;
        }
      }

      // Remove the out-root.build file and try to remove the bootstrap/
      // directory if it is empty.
      //
      r = rmfile (ctx, src_root / rs.root_extra->out_root_file)    || r;
      r = rmdir  (ctx, src_root / rs.root_extra->bootstrap_dir, 2) || r;

      return r;
    }

    static void
    disfigure_pre (context&, const values& params, const location& l)
    {
      forward (params, "disfigure", l); // Validate.
    }

    static operation_id
    disfigure_operation_pre (context&, const values&, operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means disfigure everything.
      //
      return o;
    }

    static void
    disfigure_load (const values&,
                    scope& root,
                    const path&,
                    const dir_path&,
                    const dir_path&,
                    const location&)
    {
      // Since we don't load buildfiles during disfigure but still want to
      // disfigure all the subprojects (see disfigure_project() below), we
      // bootstrap all the known subprojects.
      //
      create_bootstrap_inner (root);
    }

    static void
    disfigure_search (const values&,
                      const scope& rs,
                      const scope&,
                      const path&,
                      const target_key&,
                      const location&,
                      action_targets& ts)
    {
      ts.push_back (&rs);
    }

    static void
    disfigure_match (const values&, action, action_targets&, uint16_t, bool)
    {
    }

    static void
    disfigure_execute (const values& params,
                       action a,
                       action_targets& ts,
                       uint16_t diag,
                       bool)
    {
      tracer trace ("disfigure_execute");

      bool fwd (forward (params));

      project_set projects;

      // Note: doing everything in the load phase (disfigure_project () does
      // modify the build state).
      //
      for (const action_target& at: ts)
      {
        const scope& rs (at.as<scope> ());

        if (!(fwd
              ? disfigure_forward (   rs, projects)
              : disfigure_project (a, rs, projects)))
        {
          // Create a dir{$out_root/} target to signify the project's root in
          // diagnostics. Not very clean but seems harmless.
          //
          target& t (
            rs.ctx.targets.insert (dir::static_type,
                                   fwd ? rs.src_path () : rs.out_path (),
                                   dir_path (), // Out tree.
                                   "",
                                   nullopt,
                                   target_decl::implied,
                                   trace).first);

          if (verb != 0 && diag >= 2)
            info << diag_done (a, t);
        }
      }
    }

    const meta_operation_info mo_disfigure {
      disfigure_id,
      "disfigure",
      "disfigure",
      "disfiguring",
      "disfigured",
      "is disfigured",
      false,         // bootstrap_outer
      disfigure_pre, // meta-operation pre
      &disfigure_operation_pre,
      &disfigure_load,
      &disfigure_search,
      &disfigure_match,
      &disfigure_execute,
      nullptr, // operation post
      nullptr, // meta-operation post
      nullptr  // include
    };

    // create
    //
    static void
    save_config (context& ctx, const dir_path& d)
    {
      // Since there aren't any sub-projects yet, any config.import.* values
      // that the user may want to specify won't be saved in config.build. So
      // we go ahead and add them to config.config.persist (unless overriden).
      // To do this, however, we need the project's root scope (which is where
      // this information is stored). So what we are going to do is bootstrap
      // the newly created project, similar to the way main() does it.
      //
      scope& rs (load_project (ctx, d, d, false /* fwd */, false /* load */));

      // Add the default config.config.persist value unless there is a custom
      // one (specified as a command line override).
      //
      // Note: go straight for the public variable pool.
      //
      const variable& var (*ctx.var_pool.find ("config.config.persist"));

      if (!rs[var].defined ())
      {
        rs.assign (var) = vector<pair<string, string>> {
          pair<string, string> {"config.import.*", "unused=save"}};
      }
    }

    const string&
    preprocess_create (context& ctx,
                       values& params,
                       vector_view<opspec>& spec,
                       bool lifted,
                       const location& l)
    {
      tracer trace ("preprocess_create");

      // The overall plan is to create the project(s), update the buildspec,
      // clear the parameters, and then continue as if we were the configure
      // meta-operation.

      // Start with process parameters. The first parameter, if any, is a list
      // of root.build modules. The second parameter, if any, is a list of
      // bootstrap.build modules. If the second is not specified, then the
      // default is test, dist, and install (config is mandatory).
      //
      strings bmod {"test", "dist", "install"};
      strings rmod;
      try
      {
        size_t n (params.size ());

        if (n > 0)
          rmod = convert<strings> (move (params[0]));

        if (n > 1)
          bmod = convert<strings> (move (params[1]));

        if (n > 2)
          fail (l) << "unexpected parameters for meta-operation create";
      }
      catch (const invalid_argument& e)
      {
        fail (l) << "invalid module name: " << e.what ();
      }

      ctx.current_oname = empty_string; // Make sure valid.

      // Now handle each target in each operation spec.
      //
      for (const opspec& os: spec)
      {
        // First do some sanity checks: there should be no explicit operation
        // and our targets should all be directories.
        //
        if (!lifted && !os.name.empty ())
          fail (l) << "explicit operation specified for meta-operation create";

        for (const targetspec& ts: os)
        {
          const name& tn (ts.name);

          // Figure out the project directory. This logic must be consistent
          // with find_target_type() and other places (grep for "..").
          //
          dir_path d;

          if (tn.simple () &&
              (tn.empty () || tn.value == "." || tn.value == ".."))
            d = dir_path (tn.value);
          else if (tn.directory ())
            d = tn.dir;
          else if (tn.typed () && tn.type == "dir")
            d = tn.dir / dir_path (tn.value);
          else
            fail(l) << "non-directory target '" << ts << "' in "
                    << "meta-operation create";

          if (d.relative ())
            d = work / d;

          d.normalize (true);

          // If src_base was explicitly specified, make sure it is the same as
          // the project directory.
          //
          if (!ts.src_base.empty ())
          {
            dir_path s (ts.src_base);

            if (s.relative ())
              s = work / s;

            s.normalize (true);

            if (s != d)
              fail(l) << "different src/out directories for target '" << ts
                      << "' in meta-operation create";
          }

          l5 ([&]{trace << "creating project in " << d;});

          // For now we disable amalgamating this project. Sooner or later
          // someone will probably want to do this, though (i.e., nested
          // configurations).
          //
          create_project (d,
                          dir_path (),        /* amalgamation */
                          bmod,
                          "",                 /* root_pre */
                          rmod,
                          "",                 /* root_post */
                          string ("config"),  /* config_module */
                          nullopt,            /* config_file */
                          true,               /* buildfile */
                          "the create meta-operation",
                          1 /* verbosity */);

          save_config (ctx, d);
        }
      }

      params.clear ();
      return mo_configure.name;
    }
  }
}
