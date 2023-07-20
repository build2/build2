// file      : libbuild2/build/script/script.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/script.hxx>

#include <libbutl/filesystem.hxx>

#include <libbuild2/target.hxx>

#include <libbuild2/adhoc-rule-buildscript.hxx> // include_unmatch*

#include <libbuild2/script/timeout.hxx>

#include <libbuild2/build/script/parser.hxx>

using namespace std;

namespace build2
{
  namespace build
  {
    namespace script
    {
      using build2::script::to_deadline;

      // environment
      //
      static const optional<string> wd_name ("current directory");

      environment::
      environment (action a,
                   const target_type& t,
                   const scope_type& s,
                   bool temp,
                   const optional<timestamp>& dl)
          : build2::script::environment (
              t.ctx,
              *t.ctx.build_host,
              dir_name_view (&work, &wd_name),
              temp_dir.path, false /* temp_dir_keep */,
              redirect (redirect_type::none),
              redirect (redirect_type::merge, 2),
              redirect (redirect_type::pass)),
            target (t),
            scope (s),
            vars (context, false /* shared */), // Note: managed.
            var_ts (var_pool.insert (">")),
            var_ps (var_pool.insert ("<")),
            script_deadline (to_deadline (dl, false /* success */))
      {
        set_special_variables (a);

        if (temp)
          set_temp_dir_variable ();
      }

      void environment::
      set_special_variables (action a)
      {
        {
          // $>
          //
          // What should it contain for an explicit group? While it may seem
          // that just the members should be enough (and analogous to the ad
          // hoc case), this won't let us get the group name for diagnostics.
          // So the group name followed by all the members seems like the
          // logical choice.
          //
          names ns;

          if (const group* g = target.is_a<group> ())
          {
            g->as_name (ns);
            for (const target_type* m: g->members)
              m->as_name (ns);
          }
          else
          {
            for (const target_type* m (&target);
                 m != nullptr;
                 m = m->adhoc_member)
              m->as_name (ns);
          }

          assign (var_ts) = move (ns);
        }

        {
          // $<
          //
          // Note that ad hoc prerequisites don't end up in $<. While at first
          // thought ad hoc prerequisites in ad hoc recipes don't seem to make
          // much sense, they could be handy to exclude certain prerequisites
          // from $< while still treating them as such, especially in rule.
          //
          // While initially we treated update=unmatch prerequisites as
          // implicitly ad hoc, this turned out to be not quite correct, so
          // now we add them unless they are explicitly marked ad hoc.
          //
          names ns;
          for (const prerequisite_target& p: target.prerequisite_targets[a])
          {
            // See adhoc_buildscript_rule::execute_update_prerequisites().
            //
            if (const target_type* pt =
                p.target != nullptr ? (p.adhoc () ? nullptr : p.target) :
                (p.include & adhoc_buildscript_rule::include_unmatch) != 0 &&
                (p.include & prerequisite_target::include_adhoc) == 0      &&
                (p.include & adhoc_buildscript_rule::include_unmatch_adhoc) == 0
                ? reinterpret_cast<target_type*> (p.data)
                : nullptr)
            {
              pt->as_name (ns);
            }
          }

          assign (var_ps) = move (ns);
        }
      }

      void environment::
      set_temp_dir_variable ()
      {
        // Note that the temporary directory could have been created
        // implicitly by the runner.
        //
        if (temp_dir.path.empty ())
          create_temp_dir ();

        assign (var_pool.insert<dir_path> ("~")) = temp_dir.path;
      }

      void environment::
      create_temp_dir ()
      {
        // Create the temporary directory for this run regardless of the
        // dry-run mode, since some commands still can be executed (see run()
        // for details). This is also the reason why we are not using the
        // build2 filesystem API that considers the dry-run mode.
        //
        // Note that the directory auto-removal is active.
        //
        dir_path& td (temp_dir.path);

        assert (td.empty ()); // Must be called once.

        try
        {
          td = dir_path::temp_path ("buildscript");
        }
        catch (const system_error& e)
        {
          fail << "unable to obtain temporary directory for buildscript "
               << "execution" << e;
        }

        mkdir_status r;

        try
        {
          r = try_mkdir (td);
        }
        catch (const system_error& e)
        {
          fail << "unable to create temporary directory '" << td << "': "
               << e << endf;
        }

        // Note that the temporary directory can potentially stay after some
        // abnormally terminated script run. Clean it up and reuse if that's
        // the case.
        //
        if (r == mkdir_status::already_exists)
        try
        {
          butl::rmdir_r (td, false /* dir */);
        }
        catch (const system_error& e)
        {
          fail << "unable to cleanup temporary directory '" << td << "': "
               << e;
        }

        if (verb >= 3)
          text << "mkdir " << td;
      }

      void environment::
      set_variable (string nm,
                    names&& val,
                    const string& attrs,
                    const location& ll)
      {
        // Check if we are trying to modify any of the special variables.
        //
        if (parser::special_variable (nm))
          fail (ll) << "attempt to set '" << nm << "' special variable";

        // Set the variable value and attributes.
        //
        const variable& var (var_pool.insert (move (nm)));

        value& lhs (assign (var));

        // If there are no attributes specified then the variable assignment
        // is straightforward. Otherwise we will use the build2 parser helper
        // function.
        //
        if (attrs.empty ())
          lhs.assign (move (val), &var);
        else
        {
          // If there is an error in the attributes string, our diagnostics
          // will look like this:
          //
          // <attributes>:1:1 error: unknown value attribute x
          //   buildfile:10:1 info: while parsing attributes '[x]'
          //
          // Note that the attributes parsing error is the only reason for a
          // failure.
          //
          auto df = make_diag_frame (
            [attrs, &ll](const diag_record& dr)
            {
              dr << info (ll) << "while parsing attributes '" << attrs << "'";
            });

          parser p (context);
          p.apply_value_attributes (&var,
                                    lhs,
                                    value (move (val)),
                                    attrs,
                                    token_type::assign,
                                    path_name ("<attributes>"));
        }
      }

      lookup environment::
      lookup (const variable& var) const
      {
        auto p (vars.lookup (var));
        if (p.first != nullptr)
          return lookup_type (*p.first, p.second, vars);

        return lookup_in_buildfile (var.name);
      }

      lookup environment::
      lookup (const string& name) const
      {
        // Every variable that is ever set in a script has been added during
        // variable line execution or introduced with the set builtin. Which
        // means that if one is not found in the environment pool then it can
        // only possibly be set in the buildfile.
        //
        const variable* pvar (var_pool.find (name));
        return pvar != nullptr ? lookup (*pvar) : lookup_in_buildfile (name);
      }

      lookup environment::
      lookup_in_buildfile (const string& n) const
      {
        // Switch to the corresponding buildfile variable. Note that we don't
        // want to insert a new variable into the pool (we might be running
        // in parallel). Plus, if there is no such variable, then we cannot
        // possibly find any value.
        //
        const variable* pvar (scope.var_pool ().find (n));

        if (pvar == nullptr)
          return lookup_type ();

        return target[*pvar];
      }

      value& environment::
      append (const variable& var)
      {
        auto l (lookup (var));

        if (l.defined () && l.belongs (*this)) // Existing var.
          return vars.modify (l);

        value& r (assign (var)); // NULL.

        if (l.defined ())
          r = *l; // Copy value (and type) from the outer scope.

        return r;
      }

      void environment::
      set_timeout (const string& t, bool success, const location& l)
      {
        fragment_deadline =
          to_deadline (
            parse_deadline (t, "buildscript timeout", "timeout: ", l),
            success);
      }

      optional<deadline> environment::
      effective_deadline ()
      {
        return earlier (script_deadline, fragment_deadline);
      }
    }
  }
}
