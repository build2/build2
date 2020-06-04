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
        script
        pre_parse (const target&,
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
        pre_parse_line (token&, token_type&, bool if_line = false);

        void
        pre_parse_if_else (token&, token_type&);

        command_expr
        parse_command_line (token&, token_type&);

        // Execute. Issue diagnostics and throw failed in case of an error.
        //
      public:
        void
        execute (const scope& root, const scope& base,
                 environment&, const script&, runner&);

        // Parse a special builtin line into names, performing the variable
        // and pattern expansions. If omit_builtin is true, then omit the
        // builtin name from the result.
        //
        names
        execute_special (const scope& root, const scope& base,
                         environment&,
                         const line&,
                         bool omit_builtin = true);

      protected:
        void
        exec_script ();

        // Helpers.
        //
      public:
        static bool
        special_variable (const string&) noexcept;

        // Customization hooks.
        //
      protected:
        virtual lookup
        lookup_variable (name&&, string&&, const location&) override;

        // During execution translate the process path and executable targets
        // leaving the rest for the base parser to handle.
        //
        // During pre-parsing try to deduce the low-verbosity script
        // diagnostics name as a program/builtin name or obtain the custom
        // low-verbosity diagnostics specified with the diag builtin. Note
        // that the diag builtin can only appear at the beginning of the
        // command line.
        //
        virtual optional<process_path>
        parse_program (token&, build2::script::token_type&,
                       bool first,
                       names&) override;

      protected:
        script* script_;

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
        // diagnostics weight is set to 4.
        //
        // Any attempt to manually set the custom diagnostics twice (the diag
        // builtin after the script name or after another diag builtin) is
        // reported as ambiguity.
        //
        // At the end of pre-parsing either diag_name_ or diag_line_ (but not
        // both) are present.
        //
        optional<pair<string, location>> diag_name_;
        optional<pair<string, location>> diag_name2_; // Ambiguous script name.
        optional<pair<line, location>>   diag_line_;
        uint8_t                          diag_weight_ = 0;

        // Custom dependency change tracking.
        //
        // The depdb builtin can be used to change the default dependency
        // change tracking:
        //
        // depdb clear         - Cancels the default variables, targets, and
        //                       prerequisites change tracking. Can only be
        //                       the first depdb builtin call.
        //
        // depdb hash <args>  - Track the argument list change as a hash.
        //
        // depdb string <arg> - Track the argument (single) change as string.
        //
        optional<location> depdb_clear_; // 'depdb clear' location if any.
        lines              depdb_lines_; // Note: excludes 'depdb clear'.

        // True during pre-parsing when the pre-parse mode is temporarily
        // suspended to perform expansion.
        //
        bool pre_parse_suspended_ = false;

        // The alternative location where the next line should be saved.
        //
        // Before the script line gets parsed, it is set to a temporary value
        // that will by default be appended to the script. However,
        // parse_program() can point it to a different location where the line
        // should be saved instead (e.g., diag_line_, etc) or set it to NULL
        // if the line is handled in an ad-hoc way and should be dropped
        // (e.g., depdb_clear_, etc).
        //
        line* save_line_;

        // Execute state.
        //
        runner* runner_;
        environment* environment_;
      };
    }
  }
}

#endif // LIBBUILD2_BUILD_SCRIPT_PARSER_HXX
