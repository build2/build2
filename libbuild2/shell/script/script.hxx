// file      : libbuild2/shell/script/script.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SHELL_SCRIPT_SCRIPT_HXX
#define LIBBUILD2_SHELL_SCRIPT_SCRIPT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/variable.hxx>
#include <libbuild2/filesystem.hxx> // auto_rmdir

#include <libbuild2/script/script.hxx>

namespace build2
{
  namespace shell
  {
    namespace script
    {
      using build2::script::line;
      using build2::script::line_type;
      using build2::script::lines;
      using build2::script::end_line;
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

      class parser; // Required by VC for 'friend class parser' declaration.

      // Notes:
      //
      // - Once parsed, the script can be executed in multiple threads with
      //   the state (variable values, etc) maintained in the environment.
      //
      // - The default script command redirects semantics is 'pass' for all
      //   the standard streams.
      //
      class script
      {
      public:
        // Note that the variables are not pre-entered into a pool during the
        // parsing phase, so the line variable pointers are NULL.
        //
        // Note that in contrast to the testscript we have per-environment
        // variable pools and thus don't need to share them between multiple
        // threads (which is the reason for the testscript to pre-enter
        // variables during pre-parsing; see
        // test::script::parser::pre_parse_line() for details).
        //
        lines body;

        location start_loc;
        location end_loc;

        uint64_t syntax = 0; // Note: set to valid value by parser::pre_parse().

        script () = default;

        // Move-constructible but not move-assignable.
        //
        script (script&&) = default;
        script& operator= (script&&) = delete;

        script (const script&) = delete;
        script& operator= (const script&) = delete;

        // Pre-parse data.
        //
      private:
        friend class parser;

        // Shellscript file path names. Specifically, replay_token::file and
        // *_loc members point to these path names.
        //
        // Note: the pointers are stable since point to values in std::set.
        //
        struct compare_path_name_values
        {
          bool operator() (const path_name_value& x,
                           const path_name_value& y) const
          {
            return x.path < y.path || (x.path == y.path && x.name < y.name);
          }
        };

        set<path_name_value, compare_path_name_values> paths_;
      };

      class LIBBUILD2_SYMEXPORT environment: public build2::script::environment
      {
      public:
        using scope_type = build2::scope;

        // Use the script and args arguments to compose the $* and $N variable
        // values.
        //
        // @@ Can the script come from a stream, not a file. If yes, what the
        //    script argument (which is used as the $0 value) should refer to
        //    in this case? Note that for bash $0 refers to the bash program
        //    path in this case.
        //
        environment (const scope_type& global_scope,
                     path script,
                     strings args,
                     const optional<timestamp>& deadline = nullopt);

        environment (environment&&) = delete;
        environment (const environment&) = delete;
        environment& operator= (environment&&) = delete;
        environment& operator= (const environment&) = delete;

      public:
        // Global scope.
        //
        const scope_type& scope;

        // Script-private variable pool and map.
        //
        // Note that trying to lookup the variable in the map by using its
        // name as a string will end up with an assertion failure.
        //
        variable_pool var_pool;
        variable_map vars;

        const variable& cmd_var;      // $*
        const variable* cmdN_var[10]; // $N
        const variable& wd_var;       // $~

        // Temporary directory for the script run.
        //
        // Currently this directory is removed regardless of the script
        // execution success or failure. Later, to help with troubleshooting,
        // we may invent an option that suppresses the removal of temporary
        // files in general.
        //
        // @@ This directory is available to the user via the ??? special
        //    variable. Note, however, that the following filesystem entry
        //    prefixes are reserved:
        //
        //    stdin*
        //    stdout*
        //    stderr*
        //
        auto_rmdir temp_dir;

        // The whole script and the remaining script fragment execution
        // deadlines (the latter is set by the timeout builtin).
        //
        optional<deadline> script_deadline;
        optional<deadline> fragment_deadline;

        // Index of the next script line to be executed. Used and incremented
        // by the parser's execute() function calls to produce special file
        // names, etc.
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

        virtual void
        sleep (const duration&) override;

        // Variables.
        //
      public:
        // Lookup the variable starting from this environment and then the
        // global scope.
        //
        using lookup_type = build2::lookup;

        lookup_type
        lookup (const variable&) const;

        // @@ May add the temporary directory special variable on demand. Make
        //    it non-const if/when support for this variable is added.
        //
        lookup_type
        lookup (const string&) const;

        // As above but only look for global variables.
        //
        lookup_type
        lookup_global (const string&) const;

        // Return a value suitable for assignment. If the variable does not
        // exist in this environment's variable map, then a new one with the
        // NULL value is added and returned. Otherwise the existing value is
        // returned.
        //
        value&
        assign (const variable& var) {return vars.assign (var);}

        // Return a value suitable for append/prepend. If the variable does
        // not exist in this environment's variable map, then the global scope
        // is searched for the same variable. If found then a new variable
        // with the found value is added to the environment and returned.
        // Otherwise this function proceeds as assign() above.
        //
        value&
        append (const variable&);
      };
    }
  }
}

#endif // LIBBUILD2_SHELL_SCRIPT_SCRIPT_HXX
