// file      : libbuild2/build/script/runner.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/runner.hxx>

#include <libbutl/filesystem.hxx> // try_rmdir()

#include <libbuild2/target.hxx>
#include <libbuild2/script/run.hxx>

using namespace butl;

namespace build2
{
  namespace build
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
        // Drop cleanups of target paths.
        //
        for (auto i (env.cleanups.begin ()); i != env.cleanups.end (); )
        {
          const target* m (&env.target);
          for (; m != nullptr; m = m->adhoc_member)
          {
            if (const path_target* pm = m->is_a<path_target> ())
              if (i->path == pm->path ())
                break;
          }

          if (m != nullptr)
            i = env.cleanups.erase (i);
          else
            ++i;
        }

        clean (env, ll);

        // Remove the temporary directory, if created.
        //
        const dir_path& td (env.temp_dir.path);

        if (!td.empty ())
        {
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
           const location& ll)
      {
        if (verb >= 3)
          text << ":  " << expr;

        // Run the expression if we are not in the dry-run mode or if it
        // executes the set or exit builtin and just print the expression
        // otherwise at verbosity level 2 and up.
        //
        if (!env.context.dry_run ||
            find_if (expr.begin (), expr.end (),
                     [] (const expr_term& et)
                     {
                       const process_path& p (et.pipe.back ().program);
                       return p.initial == nullptr &&
                              (p.recall.string () == "set" ||
                               p.recall.string () == "exit");
                     }) != expr.end ())
          build2::script::run (env, expr, ii, li, ll);
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
