// file      : libbuild2/test/script/runner.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/script/runner.hxx>

#include <libbuild2/filesystem.hxx>

#include <libbuild2/script/run.hxx>

#include <libbuild2/test/common.hxx>

namespace build2
{
  namespace test
  {
    namespace script
    {
      using namespace build2::script;

      bool default_runner::
      test (scope& s) const
      {
        return common_.test (s.root.test_target, s.id_path);
      }

      pair<const process_path*, const strings*> default_runner::
      test_runner ()
      {
        return make_pair (common_.runner_path, common_.runner_options);
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

        // Note that we could probably keep the test programs sets fully
        // independent across the scopes and check if the program is a test by
        // traversing the scopes upwards recursively. Note though, that the
        // parent scope's set cannot change during the nested scope execution
        // and normally contains just a single entry. Thus, it seems more
        // efficient to get rid of the recursion by copying the set from the
        // parent now and potentially changing it later on the test variable
        // assignment, etc.
        //
        if (sp.parent != nullptr)
          sp.test_programs = sp.parent->test_programs;

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
            *sp.work_dir.path,
            sp.root.target_scope.root_scope ()->root_extra->buildignore_file,
            2)
          : mkdir (*sp.work_dir.path, 2));

        if (r == mkdir_status::already_exists)
          fail << diag_path (sp.work_dir) << " already exists" <<
            info << "are tests stomping on each other's feet?";

        // We don't change the current directory here but indicate that the
        // scope test commands will be executed in that directory.
        //
        if (verb >= 2)
          text << "cd " << *sp.work_dir.path;
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
                                  *sp.work_dir.path,
                                  sp.root.target_scope.root_scope ()->
                                    root_extra->buildignore_file,
                                  2)
            : rmdir (ctx, *sp.work_dir.path, 2));

          if (r != rmdir_status::success)
          {
            diag_record dr (fail (ll));

            dr << diag_path (sp.work_dir)
               << (r == rmdir_status::not_exist
                   ? " does not exist"
                   : " is not empty");

            if (r == rmdir_status::not_empty)
              print_dir (dr, *sp.work_dir.path, ll);
          }
        }

        // Return to the parent scope directory or to the out_base one for the
        // script scope.
        //
        if (verb >= 2)
          text << "cd " << (sp.parent != nullptr
                            ? *sp.parent->work_dir.path
                            : sp.work_dir.path->directory ());
      }

      void default_runner::
      run (scope& sp,
           const command_expr& expr, command_type ct,
           const iteration_index* ii, size_t li,
           const function<command_function>& cf,
           const location& ll)
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

        // Print test id once per test expression and only for the topmost
        // one.
        //
        auto df = make_diag_frame (
          [&sp, print = (sp.exec_level == 0)](const diag_record& dr)
          {
            if (print)
            {
              // Let's not depend on how the path representation can be
              // improved for readability on printing.
              //
              dr << info << "test id: " << sp.id_path.posix_string ();
            }
          });

        ++sp.exec_level;
        build2::script::run (sp, expr, ii, li, ll, cf);
        --sp.exec_level;
      }

      bool default_runner::
      run_cond (scope& sp,
                const command_expr& expr,
                const iteration_index* ii, size_t li,
                const location& ll)
      {
        if (verb >= 3)
          text << ": ?" << expr;

        // Print test id once per test expression and only for the topmost
        // one.
        //
        auto df = make_diag_frame (
          [&sp, print = (sp.exec_level == 0)](const diag_record& dr)
          {
            if (print)
            {
              // Let's not depend on how the path representation can be
              // improved for readability on printing.
              //
              dr << info << "test id: " << sp.id_path.posix_string ();
            }
          });

        ++sp.exec_level;
        bool r (build2::script::run_cond (sp, expr, ii, li, ll));
        --sp.exec_level;

        return r;
      }
    }
  }
}
