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
                   optional<string> diag, const location& diag_loc);

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
        // diagnostics name.
        //
        virtual optional<process_path>
        parse_program (token&, build2::script::token_type&, names&) override;

        void
        parse_program_diag (token&, build2::script::token_type&, names&);

      protected:
        script* script_;

        // Current low-verbosity script diagnostics name and weight.
        //
        // During pre-parsing each command leading names are translated into a
        // potential script name, unless it is set manually (with the diag
        // directive or via the constructor). The potential script name has a
        // weight associated with it, so script names with greater weights
        // override names with lesser weights. The possible weights are:
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
        optional<pair<string, location>> diag;
        optional<pair<string, location>> diag2;
        uint8_t                          diag_weight = 0;

        // True during pre-parsing when the pre-parse mode is temporarily
        // suspended to perform expansion.
        //
        bool pre_parse_suspended_ = false;

        // Execute state.
        //
        runner* runner_;
        environment* environment_;
      };
    }
  }
}

#endif // LIBBUILD2_BUILD_SCRIPT_PARSER_HXX
