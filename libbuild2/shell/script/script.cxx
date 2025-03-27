// file      : libbuild2/shell/script/script.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/shell/script/script.hxx>

#ifndef _WIN32
#  include <thread> // this_thread::sleep_for()
#else
#  include <libbutl/win32-utility.hxx>

#  include <chrono>
#endif

#include <libbutl/filesystem.hxx>

#include <libbuild2/scope.hxx>

#include <libbuild2/script/timeout.hxx>

#include <libbuild2/shell/script/parser.hxx>

using namespace std;

namespace build2
{
  namespace shell
  {
    namespace script
    {
      using build2::script::to_deadline;

      // environment
      //
      static const optional<string> wd_name ("current directory");

      // Note that if/when we add support for changing the working directory
      // during the script execution, then, similar to the Testscript, the
      // environment::work_dir member should refer to the $~ variable's map
      // entry and the thread-specific current directory override should be
      // used.
      //
      environment::
      environment (const scope_type& gs,
                   path p,
                   strings args,
                   const optional<timestamp>& dl)
          : build2::script::environment (
              gs.ctx.sched == nullptr || gs.ctx.sched->serial (),
              gs.ctx.no_diag_buffer,
              *gs.ctx.build_host,
              dir_name_view (&work, &wd_name),
              temp_dir.path, false /* temp_dir_keep */,
              false /* default_cleanup */,
              redirect (redirect_type::pass),
              redirect (redirect_type::pass),
              redirect (redirect_type::pass)),
            scope (gs),
            vars (gs.ctx, false /* shared */), // Note: managed.
            cmd_var (var_pool.insert<strings> ("*")),
            cmdN_var {
              &var_pool.insert<path> ("0"),
              &var_pool.insert<string> ("1"),
              &var_pool.insert<string> ("2"),
              &var_pool.insert<string> ("3"),
              &var_pool.insert<string> ("4"),
              &var_pool.insert<string> ("5"),
              &var_pool.insert<string> ("6"),
              &var_pool.insert<string> ("7"),
              &var_pool.insert<string> ("8"),
              &var_pool.insert<string> ("9")},
            wd_var (var_pool.insert<dir_path> ("~")),
            script_deadline (to_deadline (dl, false /* success */))
      {
        // Set the special $*, $N, and $~ variables.
        //
        // First assemble the $* value.
        //
        size_t n (args.size () + 1);

        strings s;
        s.reserve (n);

        s.push_back (p.representation ());

        s.insert (s.end (),
                  make_move_iterator (args.begin ()),
                  make_move_iterator (args.end ()));

        // Set the $N values.
        //
        // Note that we also set the variables which have not been passed on
        // the command line, so that they are not looked up in the global
        // scope.
        //
        for (size_t i (0); i <= 9; ++i)
        {
          value& v (assign (*cmdN_var[i]));

          if (i < n)
          {
            if (i == 0)
              v = move (p);
            else
              v = s[i];
          }
          else
            v = nullptr;
        }

        // Set $*.
        //
        assign (cmd_var) = move (s);

        // Set $~.
        //
        assign (wd_var) = work;
      }

      void environment::
      create_temp_dir ()
      {
        // Create the temporary directory for this run.
        //
        // Note that the directory auto-removal is active.
        //
        dir_path& td (temp_dir.path);

        assert (td.empty ()); // Must be called once.

        try
        {
          td = dir_path::temp_path ("shellscript");
        }
        catch (const system_error& e)
        {
          fail << "unable to obtain temporary directory for shellscript "
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
        parser::verify_variable_assignment (nm, ll);

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

          parser p (scope.ctx);
          p.apply_value_attributes (&var,
                                    lhs,
                                    value (move (val)),
                                    attrs,
                                    token_type::assign,
                                    path_name ("<attributes>"));
        }
      }

      void environment::
      sleep (const duration& d)
      {
        // Let's use scheduler if present in case this is run as a recipe.
        //
        if (scope.ctx.sched == nullptr)
        {
          // MinGW GCC 4.9 doesn't implement this_thread so use Win32 Sleep().
          //
#ifndef _WIN32
          this_thread::sleep_for (d);
#else
          using namespace chrono;
          Sleep (static_cast<DWORD> (duration_cast<milliseconds> (d).count ()));
#endif
        }
        else
          scope.ctx.sched->sleep (d);
      }

      lookup environment::
      lookup (const variable& var) const
      {
        auto p (vars.lookup (var));
        if (p.first != nullptr)
          return lookup_type (*p.first, p.second, vars);

        return lookup_global (var.name);
      }

      lookup environment::
      lookup (const string& name) const
      {
        // Every variable that is ever set in a script has been added during
        // variable line execution or introduced with the set builtin. Which
        // means that if one is not found in the environment pool then it can
        // only possibly be set in the global scope.
        //
        const variable* pvar (var_pool.find (name));

        // @@ If the not found (private) variable is the temporary directory
        //    special variable, then create the temporary directory and assign
        //    its path to the newly created variable.
        //
#if 0
        if (pvar == nullptr && name == "???")
        {
          // Note that the temporary directory could have been created
          // implicitly by the runner.
          //
          if (temp_dir.path.empty ())
            create_temp_dir ();

          pvar = &var_pool.insert<dir_path> (name);

          assign (*pvar) = temp_dir.path;
        }
#endif

        return pvar != nullptr ? lookup (*pvar) : lookup_global (name);
      }

      lookup environment::
      lookup_global (const string& n) const
      {
        // Switch to the corresponding global variable. Note that we don't
        // want to insert a new variable into the pool. Plus, if there is no
        // such variable, then we cannot possibly find any value.
        //
        const variable* pvar (scope.var_pool ().find (n));

        if (pvar == nullptr)
          return lookup_type ();

        return scope[*pvar];
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
            parse_deadline (t, "shellscript timeout", "timeout: ", l),
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
