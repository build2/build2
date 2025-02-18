// file      : libbuild2/shell/script/runner.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/shell/script/runner.hxx>

#include <libbutl/filesystem.hxx> // try_rmdir()

#include <libbuild2/scope.hxx>

#include <libbuild2/script/run.hxx>

using namespace butl;

namespace build2
{
  namespace shell
  {
    namespace script
    {
      void default_runner::
      enter (environment&, const location&)
      {
      }

      void default_runner::
      leave (environment& env, const location& ll)
      {
        clean (env, ll);

        // Remove the temporary directory, if created.
        //
        const dir_path& td (env.temp_dir.path);

        if (!td.empty ())
        {
          // @@ If we add support for $~ variable, then copy the semantics
          //    description from buildscript. Also, maybe we should just allow
          //    the script to leave the temporary files and just remove the
          //    temporary directory recursively here?
          //
          // Note that since the temporary directory may only contain special
          // files that are created and registered for cleanup by the script
          // running machinery and should all be removed by the above clean()
          // function call, its removal failure may not be the script fault
          // but potentially a bug or a filesystem problem. Thus, we don't
          // ignore the errors and report them.
          //
          env.temp_dir.cancel ();

          try
          {
            // Note that the temporary directory must be empty.
            //
            rmdir_status r (try_rmdir (td));

            if (r != rmdir_status::success)
            {
              // While there can be no fault of the script being currently
              // executed let's add the location anyway to help with
              // troubleshooting. And let's stick to that principle down the
              // road.
              //
              diag_record dr (fail (ll));
              dr << "temporary directory '" << td
                 << (r == rmdir_status::not_exist
                     ? "' does not exist"
                     : "' is not empty");

              if (r == rmdir_status::not_empty)
                build2::script::print_dir (dr, td, ll);
            }
          }
          catch (const system_error& e)
          {
            fail (ll) << "unable to remove temporary directory '" << td
                      << "': " << e;
          }

          if (verb >= 3)
            text << "rmdir " << td;
        }
      }

      void default_runner::
      run (environment& env,
           const command_expr& expr,
           const iteration_index* ii, size_t li,
           const function<command_function>& cf,
           const location& ll)
      {
        if (verb >= 3)
          text << ":  " << expr;

        // Run the expression if we are not in the dry-run mode or if it
        // executes the set or exit builtin or it is a for-loop. Otherwise,
        // just print the expression otherwise at verbosity level 2 and up.
        //
        if (!env.scope.ctx.dry_run ||
            find_if (expr.begin (), expr.end (),
                     [&cf] (const expr_term& et)
                     {
                       const process_path& p (et.pipe.back ().program);
                       return p.initial == nullptr &&
                              (p.recall.string () == "set" ||
                               p.recall.string () == "exit" ||
                               (cf != nullptr &&
                                p.recall.string () == "for"));
                     }) != expr.end ())
        {
          build2::script::run (env,
                               expr,
                               ii, li,
                               ll,
                               cf, (cf != nullptr) /* replace_last_cmd */);
        }
        else if (verb >= 2)
          text << expr;
      }

      bool default_runner::
      run_cond (environment& env,
                const command_expr& expr,
                const iteration_index* ii, size_t li,
                const location& ll)
      {
        if (verb >= 3)
          text << ": ?" << expr;

        return build2::script::run_cond (env, expr, ii, li, ll);
      }
    }
  }
}
