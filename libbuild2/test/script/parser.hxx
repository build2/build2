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
        parser (context& c): build2::script::parser (c) {}

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
        pre_parse_demote_group_scope (unique_ptr<scope>&);

        token
        pre_parse_scope_body ();

        unique_ptr<group>
        pre_parse_scope_block (token&, token_type&, const string&);

        bool
        pre_parse_line (token&, token_type&,
                        optional<description>&,
                        lines* = nullptr,
                        bool one = false,
                        optional<line_type> flow_control_type = nullopt);

        bool
        pre_parse_block_line (token&, token_type&,
                              line_type block_type,
                              optional<description>&,
                              lines&);

        bool
        pre_parse_if_else (token&, token_type&,
                           optional<description>&,
                           lines&);

        bool
        pre_parse_if_else_scope (token&, token_type&,
                                 optional<description>&,
                                 lines&);

        bool
        pre_parse_if_else_command (token&, token_type&,
                                   optional<description>&,
                                   lines&);

        bool
        pre_parse_loop (token&, token_type&,
                        line_type,
                        optional<description>&,
                        lines&);

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
