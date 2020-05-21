// file      : libbuild2/build/script/runner.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/runner.hxx>

#include <libbutl/filesystem.mxx>

#include <libbuild2/script/run.hxx>

using namespace butl;

namespace build2
{
  namespace build
  {
    namespace script
    {
      void default_runner::
      enter (environment& env, const location& ll)
      {
        // Create the temporary directory for this run regardless of the
        // dry-run mode, since some commands still can be executed (see run()
        // for details). This also a reason for not using the build2
        // filesystem API that considers the dry-run mode.
        //
        // Note that the directory auto-removal is active.
        //
        dir_path& td (env.temp_dir.path);

        try
        {
          td = dir_path::temp_path ("build2-build-script");
        }
        catch (const system_error& e)
        {
          // While there can be no fault of the script being currently
          // executed let's add the location anyway to ease the
          // troubleshooting. And let's stick to that principle down the road.
          //
          fail (ll) << "unable to obtain temporary directory for buildscript "
                    << "execution" << e;
        }

        mkdir_status r;

        try
        {
          r = try_mkdir (td);
        }
        catch (const system_error& e)
        {
          fail(ll) << "unable to create temporary directory '" << td << "': "
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
          fail (ll) << "unable to cleanup temporary directory '" << td
                    << "': " << e;
        }

        if (verb >= 3)
          text << "mkdir " << td;
      }

      void default_runner::
      leave (environment& env, const location& ll)
      {
        clean (env, ll);

        // Note that since the temporary directory may only contain special
        // files that are created and registered for cleanup by the script
        // running machinery and should all be removed by the above clean()
        // function call, its removal failure may not be the script fault but
        // potentially a bug or a filesystem problem. Thus, we don't ignore
        // the errors and report them.
        //
        env.temp_dir.cancel ();

        const dir_path& td (env.temp_dir.path);

        try
        {
          // Note that the temporary directory must be empty to date.
          //
          rmdir_status r (try_rmdir (td));

          if (r != rmdir_status::success)
          {
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
          fail (ll) << "unable to remove temporary directory '" << td << "': "
                    << e;
        }

        if (verb >= 3)
          text << "rmdir " << td;
      }

      void default_runner::
      run (environment& env,
           const command_expr& expr,
           size_t li,
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
                       const string& p (et.pipe.back ().program.string ());
                       return p == "set" || p == "exit";
                     }) != expr.end ())
          build2::script::run (env, expr, li, ll);
        else if (verb >= 2)
          text << expr;
      }

      bool default_runner::
      run_if (environment& env,
              const command_expr& expr,
              size_t li, const location& ll)
      {
        if (verb >= 3)
          text << ": ?" << expr;

        return build2::script::run_if (env, expr, li, ll);
      }
    }
  }
}
