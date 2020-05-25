// file      : libbuild2/build/script/script.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BUILD_SCRIPT_SCRIPT_HXX
#define LIBBUILD2_BUILD_SCRIPT_SCRIPT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/variable.hxx>
#include <libbuild2/filesystem.hxx> // auto_rmdir

#include <libbuild2/script/script.hxx>

namespace build2
{
  namespace build
  {
    namespace script
    {
      using build2::script::line;
      using build2::script::line_type;
      using build2::script::redirect;
      using build2::script::redirect_type;
      using build2::script::expr_term;
      using build2::script::command_expr;

      // Notes:
      //
      // - Once parsed, the script can be executed in multiple threads with
      //   the state (variable values, etc) maintained in the environment
      //   object.
      //
      // - The default script command redirects semantics is none for stdin,
      //   merge into stderr for stdout, and pass for stderr.
      //
      class script
      {
      public:
        // Note that the variables are not pre-entered into a pool during the
        // parsing phase, so the line variable pointers are NULL.
        //
        build2::script::lines   lines;

        // Referenced ordinary (non-special) variables.
        //
        // Used for the script semantics change tracking. The variable list is
        // filled during the pre-parsing phase and is checked against during
        // the execution phase. If during execution some non-script-local
        // variable is not found in the list (may happen for a computed name),
        // then the execution fails since the script semantics may not be
        // properly tracked (the variable value change will not trigger the
        // target rebuild).
        //
        small_vector<string, 1> vars;

        location start_loc;
        location end_loc;
      };

      class environment: public build2::script::environment
      {
      public:
        environment (const target& primary_target);

        environment (environment&&) = delete;
        environment (const environment&) = delete;
        environment& operator= (environment&&) = delete;
        environment& operator= (const environment&) = delete;

      public:
        const target& primary_target;

        // Script-local variable pool.
        //
        variable_pool var_pool;

        // Note that if we pass the variable name as a string, then it will
        // be looked up in the wrong pool.
        //
        variable_map vars;

        // Temporary directory for the script run.
        //
        // Currently this directory is removed regardless of the script
        // execution success or failure. Later, to help with troubleshooting,
        // we may invent an option that suppresses the removal of temporary
        // files in general.
        //
        // This directory is available to the user via the $~ special
        // variable. Note, however, that the following filesystem entry
        // prefixes are reserved:
        //
        // stdin*
        // stdout*
        // stderr*
        //
        auto_rmdir temp_dir;

        virtual void
        set_variable (string&& name, names&&, const string& attrs) override;

        // Variables.
        //
      public:
        // Lookup the variable starting from this environment, then the
        // primary target, and then outer buildfile scopes.
        //
        using lookup_type = build2::lookup;

        lookup_type
        lookup (const variable&) const;

        lookup_type
        lookup (const string&) const;

        // As above but only look for buildfile variables.
        //
        lookup_type
        lookup_in_buildfile (const string&) const;

        // Return a value suitable for assignment. If the variable does not
        // exist in this environment's map, then a new one with the NULL value
        // is added and returned. Otherwise the existing value is returned.
        //
        value&
        assign (const variable& var) {return vars.assign (var);}

        // Return a value suitable for append/prepend. If the variable does
        // not exist in this environment's map, then outer scopes are searched
        // for the same variable. If found then a new variable with the found
        // value is added to the environment and returned. Otherwise this
        // function proceeds as assign() above.
        //
        value&
        append (const variable&);
      };
    }
  }
}

#endif // LIBBUILD2_BUILD_SCRIPT_SCRIPT_HXX
