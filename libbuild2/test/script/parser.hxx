// file      : libbuild2/test/script/parser.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_SCRIPT_PARSER_HXX
#define LIBBUILD2_TEST_SCRIPT_PARSER_HXX

#include <unordered_map>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/diagnostics.hxx>

#include <libbuild2/script/parser.hxx>

#include <libbuild2/test/script/token.hxx>
#include <libbuild2/test/script/script.hxx>

namespace build2
{
  namespace test
  {
    namespace script
    {
      class runner;

      class parser: public build2::script::parser
      {
        // Pre-parse. Issue diagnostics and throw failed in case of an error.
        //
      public:
        parser (context& c, uint64_t syntax)
            : build2::script::parser (c, syntax) {}

        void
        pre_parse (script&);

        void
        pre_parse (istream&, script&);

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
        bool
        pre_parse_demote_group_to_test (unique_ptr<scope>&); // Syntax 1 only.

        token
        pre_parse_group_body ();

        unique_ptr<group>
        pre_parse_group_block (token&, token_type&, const string&);

        unique_ptr<test>
        pre_parse_test_block (
          token&, token_type&,
          const string&,
          pair<bool, optional<description>>* semi_colon = nullptr);

        pair<bool, optional<description>>
        pre_parse_command_block (token&, token_type&,
                                 lines&,
                                 optional<line_type> block_type,
                                 bool allow_semi_colon = true);

        pair<bool, optional<description>>
        pre_parse_command_line (token&, token_type&,
                                lines&,
                                optional<line_type> block_type);

        bool
        pre_parse_command_line_v1 (token&, token_type&,
                                   optional<description>&,
                                   lines&,
                                   line_type block_type);

        bool
        pre_parse_if_else (token&, token_type&,
                           optional<description>&,
                           lines&,
                           bool command_only);

        bool
        pre_parse_if_else_group (token&, token_type&,
                                 optional<description>&,
                                 lines&);

        pair<unique_ptr<test>, bool>
        pre_parse_if_else_test (token&, token_type&,
                                optional<description>&,
                                lines&,
                                const location&);

        bool
        pre_parse_if_else_command (token&, token_type&,
                                   optional<description>&,
                                   lines&);

        bool
        pre_parse_if_else_command_v1 (token&, token_type&,
                                      optional<description>&,
                                      lines&);

        bool
        pre_parse_loop (token&, token_type&,
                        line_type,
                        optional<description>&,
                        lines&);

        bool
        pre_parse_loop_v1 (token&, token_type&,
                           line_type,
                           optional<description>&,
                           lines&);

        bool
        pre_parse_line (token&, token_type&,
                        optional<description>&,
                        lines* = nullptr,
                        bool one = false,
                        optional<line_type> flow_control_type = nullopt,
                        bool command_only_if = false);

        void
        pre_parse_directive (token&, token_type&);

        void
        pre_parse_include_line (names, location);

        description
        pre_parse_leading_description (token&, token_type&);

        description
        parse_trailing_description (token&, token_type&);

        command_expr
        parse_command_line (token&, token_type&);

        // Verify that no teardown commands were pre-parsed yet in the current
        // group scope. If that's not the case, issue diagnostics in the
        // following form and fail:
        //
        // <location>: error: <what> after teardown
        //   <location>: info: last teardown line appears here
        //
        // This, in particular, will detect things like variable assignments
        // between scopes.
        //
        void
        verify_no_teardown (const char* what, const location&) const;

        // Execute. Issue diagnostics and throw failed in case of an error.
        //
      public:
        void
        execute (script&, runner&);

        void
        execute (scope&, script&, runner&);

      protected:
        void
        exec_scope_body ();

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

        // Insert id into the id map checking for duplicates.
        //
      protected:
        const string&
        insert_id (string, location);

      protected:
        script* script_;

        // Pre-parse state.
        //
        using id_map = std::unordered_map<string, location>;
        using include_set = set<path>;

        group* group_;
        id_map* id_map_;
        include_set* include_set_; // Testscripts already included in this
                                   // scope. Must be absolute and normalized.

        string id_prefix_; // Auto-derived id prefix.

        // Execute state.
        //
        runner* runner_;
        scope* scope_;
      };
    }
  }
}

#endif // LIBBUILD2_TEST_SCRIPT_PARSER_HXX
