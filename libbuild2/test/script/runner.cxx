// file      : libbuild2/test/script/runner.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/script/runner.hxx>

#include <libbuild2/script/run.hxx>

#include <libbuild2/test/common.hxx>

namespace build2
{
  namespace test
  {
    namespace script
    {
      bool default_runner::
      test (scope& s) const
      {
        return common_.test (s.root.test_target, s.id_path);
      }

      void default_runner::
      enter (scope& sp, const location&)
      {
        context& ctx (sp.context);

        auto df = make_diag_frame (
          [&sp](const diag_record& dr)
          {
            // Let's not depend on how the path representation can be improved
            // for readability on printing.
            //
            dr << info << "test id: " << sp.id_path.posix_string ();
          });

        // Scope working directory shall be empty (the script working
        // directory is cleaned up by the test rule prior the script
        // execution).
        //
        // Create the root working directory containing the .buildignore file
        // to make sure that it is ignored by name patterns (see buildignore
        // description for details).
        //
        // @@ Shouldn't we add an optional location parameter to mkdir() and
        // alike utility functions so the failure message can contain
        // location info?
        //
        fs_status<mkdir_status> r (
          sp.parent == nullptr
          ? mkdir_buildignore (
            ctx,
            sp.work_dir,
            sp.root.target_scope.root_scope ()->root_extra->buildignore_file,
            2)
          : mkdir (sp.work_dir, 2));

        if (r == mkdir_status::already_exists)
          fail << "working directory " << sp.work_dir << " already exists" <<
            info << "are tests stomping on each other's feet?";

        // We don't change the current directory here but indicate that the
        // scope test commands will be executed in that directory.
        //
        if (verb >= 2)
          text << "cd " << sp.work_dir;
      }

      void default_runner::
      leave (scope& sp, const location& ll)
      {
        auto df = make_diag_frame (
          [&sp](const diag_record& dr)
          {
            // Let's not depend on how the path representation can be improved
            // for readability on printing.
            //
            dr << info << "test id: " << sp.id_path.posix_string ();
          });

        // Perform registered cleanups if requested.
        //
        if (common_.after == output_after::clean)
        {
          clean (sp, ll);

          context& ctx (sp.context);

          rmdir_status r (
            sp.parent == nullptr
            ?  rmdir_buildignore (ctx,
                                  sp.work_dir,
                                  sp.root.target_scope.root_scope ()->
                                    root_extra->buildignore_file,
                                  2)
            : rmdir (ctx, sp.work_dir, 2));

          if (r != rmdir_status::success)
          {
            diag_record dr (fail (ll));
            dr << "working directory " << sp.work_dir
               << (r == rmdir_status::not_exist
                   ? " does not exist"
                   : " is not empty");

            if (r == rmdir_status::not_empty)
              build2::script::print_dir (dr, sp.work_dir, ll);
          }
        }

        // Return to the parent scope directory or to the out_base one for the
        // script scope.
        //
        if (verb >= 2)
          text << "cd " << (sp.parent != nullptr
                            ? sp.parent->work_dir
                            : sp.work_dir.directory ());
      }

      void default_runner::
      run (scope& sp,
           const command_expr& expr, command_type ct,
           size_t li, const location& ll)
      {
        // Noop for teardown commands if keeping tests output is requested.
        //
        if (ct == command_type::teardown &&
            common_.after == output_after::keep)
          return;

        if (verb >= 3)
        {
          char c ('\0');

          switch (ct)
          {
          case command_type::test:     c = ' '; break;
          case command_type::setup:    c = '+'; break;
          case command_type::teardown: c = '-'; break;
          }

          text << ": " << c << expr;
        }

        // Print test id once per test expression.
        //
        auto df = make_diag_frame (
          [&sp](const diag_record& dr)
          {
            // Let's not depend on how the path representation can be improved
            // for readability on printing.
            //
            dr << info << "test id: " << sp.id_path.posix_string ();
          });

        build2::script::run (sp, expr, li, ll);
      }

      bool default_runner::
      run_if (scope& sp,
              const command_expr& expr,
              size_t li, const location& ll)
      {
        if (verb >= 3)
          text << ": ?" << expr;

        // Print test id once per test expression.
        //
        auto df = make_diag_frame (
          [&sp](const diag_record& dr)
          {
            // Let's not depend on how the path representation can be improved
            // for readability on printing.
            //
            dr << info << "test id: " << sp.id_path.posix_string ();
          });

        return build2::script::run_if (sp, expr, li, ll);
      }
    }
  }
}
