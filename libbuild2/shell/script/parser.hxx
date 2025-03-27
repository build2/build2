// file      : libbuild2/shell/script/parser.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SHELL_SCRIPT_PARSER_HXX
#define LIBBUILD2_SHELL_SCRIPT_PARSER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/diagnostics.hxx>

#include <libbuild2/script/parser.hxx>

#include <libbuild2/shell/script/token.hxx>
#include <libbuild2/shell/script/script.hxx>

namespace build2
{
  namespace shell
  {
    namespace script
    {
      class runner;

      class LIBBUILD2_SYMEXPORT parser: public build2::script::parser
      {
        // Pre-parse. Issue diagnostics and throw failed in case of an error.
        //
      public:
        parser (const context& c)
          : build2::script::parser (c.var_pool, c.functions, 2 /* syntax */) {}

        script
        pre_parse (const scope&, const path&);

        script
        pre_parse (const scope&, istream&, const path_name&, uint64_t line);

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
        pre_parse_block_line (token&, token_type&);

        void
        pre_parse_block (token&, token_type&);

        void
        pre_parse_if_else (token&, token_type&);

        void
        pre_parse_loop (token&, token_type&);

        command_expr
        parse_command_line (token&, token_type&);

        // Execute. Issue diagnostics and throw failed in case of an error.
        //
      public:
        int
        execute (environment&, const script&, runner&);

        // Helpers.
        //
      public:
        static bool
        special_variable (const string&) noexcept;

        // Verify that variable with the specified name can be assigned. Issue
        // diagnostics and throw failed if that's not the case (this is a
        // special variable, etc).
        //
        static void
        verify_variable_assignment (const string&, const location&);

        // Customization hooks.
        //
      protected:
        virtual lookup
        lookup_variable (names&&, string&&, const location&) override;

      protected:
        script* script_;

        // Execute state.
        //
        runner* runner_;
        environment* environment_;
      };
    }
  }
}

#endif // LIBBUILD2_SHELL_SCRIPT_PARSER_HXX
