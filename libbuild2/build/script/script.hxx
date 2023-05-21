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
      using build2::script::lines;
      using build2::script::redirect;
      using build2::script::redirect_type;
      using build2::script::command;
      using build2::script::expr_term;
      using build2::script::command_expr;
      using build2::script::iteration_index;
      using build2::script::deadline;
      using build2::script::timeout;
      using build2::script::pipe_command;
      using build2::script::command_function;

      // Forward declarations.
      //
      class default_runner;

      // Notes:
      //
      // - Once parsed, the script can be executed in multiple threads with
      //   the state (variable values, etc) maintained in the environment.
      //
      // - The default script command redirects semantics is 'none' for stdin,
      //   'merge' into stderr for stdout, and 'pass' for stderr.
      //
      class script
      {
      public:
        // Note that the variables are not pre-entered into a pool during the
        // parsing phase, so the line variable pointers are NULL.
        //
        lines body;
        bool  body_temp_dir = false; // True if the body references $~.

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
        small_vector<string, 2> vars; // 2 for command and options.

        // Command name for low-verbosity diagnostics and custom low-verbosity
        // diagnostics line, potentially preceded with the variable
        // assignments. Note: cannot be both (see the script parser for
        // details).
        //
        optional<string> diag_name;
        lines            diag_preamble;
        bool             diag_preamble_temp_dir = false; // True if refs $~.

        // The script's custom dependency change tracking lines (see the
        // script parser for details).
        //
        bool             depdb_clear;
        bool             depdb_value;                    // String or hash.
        optional<size_t> depdb_dyndep;                   // Pos of first dyndep.
        bool             depdb_dyndep_byproduct = false; // dyndep --byproduct
        bool             depdb_dyndep_dyn_target = false;// dyndep --dyn-target
        lines            depdb_preamble;                 // Note include vars.
        bool             depdb_preamble_temp_dir = false;// True if refs $~.

        location start_loc;
        location end_loc;
      };

      class environment: public build2::script::environment
      {
      public:
        using scope_type = build2::scope;
        using target_type = build2::target;

        environment (action,
                     const target_type&,
                     const scope_type&,
                     bool temp_dir,
                     const optional<timestamp>& deadline = nullopt);

        // (Re)set special $< and $> variables.
        //
        void
        set_special_variables (action);

        // Create the temporary directory (if it doesn't exist yet) and set
        // the $~ special variable to its path.
        //
        void
        set_temp_dir_variable ();

        environment (environment&&) = delete;
        environment (const environment&) = delete;
        environment& operator= (environment&&) = delete;
        environment& operator= (const environment&) = delete;

      public:
        // Primary target this environment is for and its base scope;
        //
        const target_type& target;
        const scope_type&  scope;

        // Script-private variable pool and map.
        //
        // Note that it may be tempting to reuse the rule-specific variables
        // for this but they should not be modified during execution (i.e.,
        // they are for intra-rule communication; perhaps we could have a
        // special builtin that sets such variables during match).
        //
        // Note also that if we lookup the variable by passing name as a
        // string, then it will be looked up in the wrong pool.
        //
        variable_pool var_pool;
        variable_map vars;

        const variable& var_ts; // $>
        const variable& var_ps; // $<

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

        // The whole script and the remaining script fragment execution
        // deadlines (the latter is set by the timeout builtin).
        //
        optional<deadline> script_deadline;
        optional<deadline> fragment_deadline;

        // Index of the next script line to be executed. Used and incremented
        // by the parser's execute_depdb_preamble() and execute_body()
        // function calls to produce special file names, etc.
        //
        size_t exec_line = 1;

        virtual void
        set_variable (string name,
                      names&&,
                      const string& attrs,
                      const location&) override;

        // Parse the specified in seconds timeout and set the remaining script
        // fragment execution deadline. Reset it to nullopt on zero.
        //
        virtual void
        set_timeout (const string&, bool success, const location&) override;

        // Return the nearest of the script and fragment execution deadlines.
        //
        virtual optional<deadline>
        effective_deadline () override;

        virtual void
        create_temp_dir () override;

        // Variables.
        //
      public:
        // Lookup the variable starting from this environment, then the
        // primary target, and then outer buildfile scopes.
        //
        // Note that we currently skip rule-specific variables since the rule
        // that runs this script doesn't set any.
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
        // exist in this environment's variable map, then a new one with the
        // NULL value is added and returned. Otherwise the existing value is
        // returned.
        //
        value&
        assign (const variable& var) {return vars.assign (var);}

        // Return a value suitable for append/prepend. If the variable does
        // not exist in this environment's variable map, then outer scopes are
        // searched for the same variable. If found then a new variable with
        // the found value is added to the environment and returned. Otherwise
        // this function proceeds as assign() above.
        //
        value&
        append (const variable&);
      };
    }
  }
}

#endif // LIBBUILD2_BUILD_SCRIPT_SCRIPT_HXX
