// file      : libbuild2/build/script/parser.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BUILD_SCRIPT_PARSER_HXX
#define LIBBUILD2_BUILD_SCRIPT_PARSER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/diagnostics.hxx>

#include <libbuild2/script/parser.hxx>

#include <libbuild2/build/script/token.hxx>
#include <libbuild2/build/script/script.hxx>

namespace build2
{
  namespace build
  {
    namespace script
    {
      class runner;

      class parser: public build2::script::parser
      {
        // Pre-parse. Issue diagnostics and throw failed in case of an error.
        //
      public:
        parser (context& c): build2::script::parser (c) {}

        // Note that the returned script object references the passed path
        // name.
        //
        // Note also that we use the scope to lookup variable values while
        // trying to deduce the low verbosity diagnostics name (see code
        // around pre_parse_suspended for details). But that means we may
        // derive such a name based on the wrong value. This can happen if the
        // expanded variable value is reset after the recipe has been
        // pre-parsed or if such a value is set on the target (which is where
        // we start when looking up variables during the real parse). The
        // current thinking is that a remote possibility of this happening is
        // acceptable in this situation -- the worst that can happen is that
        // we will end up with mismatching diagnostics.
        //
        script
        pre_parse (const scope&,
                   const target_type&,
                   const small_vector<action, 1>&,
                   istream&, const path_name&, uint64_t line,
                   optional<string> diag_name, const location& diag_loc);

        // Recursive descent parser.
        //
        // Usually (but not always) parse functions receive the token/type
        // from which it should start consuming and in return the token/type
        // should contain the first token that has not been consumed.
        //
        // Functions that are called parse_*() rather than pre_parse_*() are
        // used for both stages.
        //
      protected:
        token
        pre_parse_script ();

        void
        pre_parse_line (token&, token_type&,
                        optional<line_type> flow_control_type = nullopt);

        void
        pre_parse_block_line (token&, token_type&, line_type block_type);

        void
        pre_parse_if_else (token&, token_type&);

        void
        pre_parse_loop (token&, token_type&, line_type);

        command_expr
        parse_command_line (token&, token_type&);

        // Execute. Issue diagnostics and throw failed in case of an error.
        //
      public:

        // By default call the runner's enter() and leave() functions that
        // initialize/clean up the environment before/after the script
        // execution.
        //
        // Note: having both root and base scopes for testing (where we pass
        // global scope for both).
        //
        void
        execute_body (const scope& root, const scope& base,
                      environment&, const script&, runner&,
                      bool enter = true, bool leave = true);

        // Execute the first or the second (dyndep) half of the depdb
        // preamble.
        //
        // Note that it's the caller's responsibility to make sure that the
        // runner's enter() function is called before the first preamble/body
        // command execution and leave() -- after the last command.
        //
        // Note: target must be file or group.
        //
        void
        execute_depdb_preamble (action a, const scope& base, const target& t,
                                environment& e, const script& s, runner& r,
                                depdb& dd)
        {
          auto b (s.depdb_preamble.begin ());
          exec_depdb_preamble (
            a, base, t,
            e, s, r,
            b,
            (s.depdb_dyndep
             ? b + *s.depdb_dyndep
             : s.depdb_preamble.end ()),
            dd);
        }

        struct dynamic_target
        {
          string       type; // Target type name (absent if static member).
          build2::path path;
        };

        using dynamic_targets = vector<dynamic_target>;

        void
        execute_depdb_preamble_dyndep (
          action a, const scope& base, target& t,
          environment& e, const script& s, runner& r,
          depdb& dd,
          dynamic_targets& dyn_targets,
          bool& update, timestamp mt, bool& deferred_failure)
        {
          exec_depdb_preamble (
            a, base, t,
            e, s, r,
            s.depdb_preamble.begin () + *s.depdb_dyndep,
            s.depdb_preamble.end (),
            dd, &dyn_targets, &update, mt, &deferred_failure);
        }

        // This version doesn't actually execute the depdb-dyndep builtin (but
        // may execute some variable assignments) instead returning all the
        // information (extracted from options) necessary to implement the
        // depdb-dyndep --byproduct logic (which fits better into the rule
        // implementation).
        //
        enum class dyndep_format {make, lines};

        struct dyndep_byproduct
        {
          location_value     location;
          dyndep_format      format;
          optional<dir_path> cwd;
          path               file;
          string             what;
          const target_type* default_type;
          bool               drop_cycles;
        };

        dyndep_byproduct
        execute_depdb_preamble_dyndep_byproduct (
          action a, const scope& base, const target& t,
          environment& e, const script& s, runner& r,
          depdb& dd, bool& update, timestamp mt)
        {
          // Dummies.
          //
          // This is getting a bit ugly (we also don't really need to pass
          // depdb here). One day we will find a better way...
          //
          dynamic_targets dyn_targets;
          bool deferred_failure;

          dyndep_byproduct v;
          exec_depdb_preamble (
            a, base, t,
            e, s, r,
            s.depdb_preamble.begin () + *s.depdb_dyndep,
            s.depdb_preamble.end (),
            dd, &dyn_targets, &update, mt, &deferred_failure, &v);
          return v;
        }

        // If the diag argument is true, then execute the preamble including
        // the (trailing) diagnostics line and return the resulting names and
        // its location (see exec_special() for the diagnostics line execution
        // semantics). Otherwise, execute the preamble excluding the
        // diagnostics line and return an empty names list and location. If
        // requested, call the runner's enter() and leave() functions that
        // initialize/clean up the environment before/after the preamble
        // execution.
        //
        // Note: having both root and base scopes for testing (where we pass
        // global scope for both).
        //
        pair<names, location>
        execute_diag_preamble (const scope& root, const scope& base,
                               environment&, const script&, runner&,
                               bool diag, bool enter, bool leave);

      protected:
        // Setup the parser for subsequent exec_*() function calls.
        //
        void
        pre_exec (const scope& root, const scope& base,
                  environment&, const script*, runner*);

        using lines_iterator = lines::const_iterator;

        void
        exec_lines (lines_iterator, lines_iterator,
                    const function<exec_cmd_function>&);

        void
        exec_lines (const lines& l, const function<exec_cmd_function>& c)
        {
          exec_lines (l.begin (), l.end (), c);
        }

        // Parse a special builtin line into names, performing the variable
        // and pattern expansions. Optionally, skip the first token (builtin
        // name, etc).
        //
        names
        exec_special (token&, build2::script::token_type&, bool skip_first);

        // Note: target must be file or group.
        //
        void
        exec_depdb_preamble (action, const scope& base, const target&,
                             environment&, const script&, runner&,
                             lines_iterator begin, lines_iterator end,
                             depdb&,
                             dynamic_targets* dyn_targets = nullptr,
                             bool* update = nullptr,
                             optional<timestamp> mt = nullopt,
                             bool* deferred_failure = nullptr,
                             dyndep_byproduct* = nullptr);

        // Note: target must be file or group.
        //
        void
        exec_depdb_dyndep (token&, build2::script::token_type&,
                           size_t line_index, const location&,
                           action, const scope& base, target&,
                           depdb&,
                           dynamic_targets& dyn_targets,
                           bool& update,
                           timestamp,
                           bool& deferred_failure,
                           dyndep_byproduct*);

        // Helpers.
        //
      public:
        static bool
        special_variable (const string&) noexcept;

        // Customization hooks.
        //
      protected:
        virtual lookup
        lookup_variable (names&&, string&&, const location&) override;

        virtual void
        lookup_function (string&&, const location&) override;

        // During execution translate the process path and executable targets
        // leaving the rest for the base parser to handle.
        //
        // During pre-parsing try to deduce the low-verbosity script
        // diagnostics name as a program/builtin name or obtain the custom
        // low-verbosity diagnostics specified with the diag builtin. Also
        // handle the depdb builtin calls.
        //
        // Note that the diag and depdb builtins can only appear at the
        // beginning of the command line.
        //
        virtual optional<process_path>
        parse_program (token&, build2::script::token_type&,
                       bool first, bool env,
                       names&, parse_names_result&) override;

      protected:
        script* script_;
        const small_vector<action, 1>* actions_; // Non-NULL during pre-parse.

        // True if this script is for file- or file group-based targets and
        // performing update is one of the actions, respectively. Only set for
        // the pre-parse mode.
        //
        bool file_based_;
        bool perform_update_;

        // Current low-verbosity script diagnostics and its weight.
        //
        // During pre-parsing each command leading names are translated into a
        // potential low-verbosity script diagnostics name, unless the
        // diagnostics is set manually (script name via the constructor or
        // custom diagnostics via the diag builtin). The potential script
        // name has a weight associated with it, so script names with greater
        // weights override names with lesser weights. The possible weights
        // are:
        //
        // 0     - builtins that do not add to the script semantics (exit,
        //         true, etc) and are never picked up as a script name
        //
        // [1 2] - other builtins
        //
        // 3     - process path or executable target
        //
        // 4     - manually set names
        //
        // If two potential script names with the same weights are encountered
        // then this ambiguity is reported unless a higher-weighted name is
        // encountered later.
        //
        // If the diag builtin is encountered, then its whole line is saved
        // (including the leading 'diag' word) for later execution and the
        // diagnostics weight is set to 4. The preceding lines, which can only
        // contain variable assignments (including via the set builtin,
        // potentially inside the flow control constructs), are also saved.
        //
        // Any attempt to manually set the custom diagnostics twice (the diag
        // builtin after the script name or after another diag builtin) is
        // reported as ambiguity.
        //
        // If no script name is deduced by the end of pre-parsing and the
        // script is used for a single operation, then use this operation's
        // name as a script name.
        //
        // At the end of pre-parsing either diag_name_ is present or
        // diag_preamble_ is not empty (but not both).
        //
        optional<pair<string, location>> diag_name_;
        optional<pair<string, location>> diag_name2_; // Ambiguous script name.
        lines                            diag_preamble_;
        uint8_t                          diag_weight_ = 0;

        // Custom dependency change tracking.
        //
        // The depdb builtin can be used to change the default dependency
        // change tracking:
        //
        // depdb clear           - Cancel the default variables, targets, and
        //                         prerequisites change tracking. Can only be
        //                         the first depdb builtin call.
        //
        // depdb hash <args>     - Track the argument list change as a hash.
        //
        // depdb string <arg>    - Track the argument (single) change as string.
        //
        // depdb env <var-names> - Track the environment variables change as a
        //                         hash.
        //
        // depdb dyndep ...      - Extract dynamic dependency information. Can
        //                         only be the last depdb builtin call in the
        //                         preamble. Note that such dependencies don't
        //                         end up in $<. We also don't cause clean of
        //                         such dependencies (since there may be no .d
        //                         file) -- they should also be listed as
        //                         static prerequisites of some other target
        //                         (e.g., lib{} for headers) or a custom clean
        //                         recipe should be provided.
        //
        //
        optional<location> depdb_clear_;         // depdb-clear location.
        bool               depdb_value_ = false; // depdb-{string,hash}
        optional<pair<location, size_t>>
                           depdb_dyndep_;   // depdb-dyndep location/position.
        bool               depdb_dyndep_byproduct_ = false; // --byproduct
        bool               depdb_dyndep_dyn_target_ = false; // --dyn-target
        lines              depdb_preamble_; // Note: excluding depdb-clear.

        // If present, the first impure function called in the body of the
        // script that performs update of a file-based target.
        //
        // Note that during the line pre-parsing we cannot tell if this is a
        // body or depdb preamble line. Thus, if we encounter an impure
        // function call we just save its name/location and postpone the
        // potential failure till the end of the script pre-parsing, if it
        // turns out to be a body line.
        //
        optional<pair<string, location>> impure_func_;

        // Similar to the impure function above but for a computed (e.g.,
        // target-qualified) variable expansion. In this case we don't have a
        // name (it's computed).
        //
        optional<location> computed_var_;

        // True if we (rather than the base parser) turned on the pre-parse
        // mode.
        //
        bool top_pre_parse_;

        // True during top-pre-parsing when the pre-parse mode is temporarily
        // suspended to perform expansion.
        //
        bool pre_parse_suspended_ = false;

        // The alternative location where the next line should be saved.
        //
        // Before the script line gets parsed, it is set to a temporary value
        // that will by default be appended to the script. However,
        // parse_program() can point it to a different location where the line
        // should be saved instead (e.g., diag_preamble_ back, etc) or set it
        // to NULL if the line is handled in an ad-hoc way and should be
        // dropped (e.g., depdb_clear_, etc).
        //
        line* save_line_;

        // The flow control constructs nesting level.
        //
        // Maintained during pre-parsing and is incremented when flow control
        // construct condition lines are encountered, which in particular
        // means that it is already incremented by the time the condition
        // expression is pre-parsed. Decremented when the cmd_end line is
        // encountered.
        //
        size_t level_ = 0;

        // Execute state.
        //
        runner* runner_;
        environment* environment_;
      };
    }
  }
}

#endif // LIBBUILD2_BUILD_SCRIPT_PARSER_HXX
