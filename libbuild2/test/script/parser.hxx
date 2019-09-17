// file      : libbuild2/test/script/parser.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_SCRIPT_PARSER_HXX
#define LIBBUILD2_TEST_SCRIPT_PARSER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/parser.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/test/script/token.hxx>
#include <libbuild2/test/script/script.hxx>

namespace build2
{
  class context;

  namespace test
  {
    namespace script
    {
      class lexer;
      class runner;

      class parser: protected build2::parser
      {
        // Pre-parse. Issue diagnostics and throw failed in case of an error.
        //
      public:
        parser (context& c): build2::parser (c) {}

        void
        pre_parse (script&);

        void
        pre_parse (istream&, script&);

        // Helpers.
        //
        // Parse attribute string and perform attribute-guided assignment.
        // Issue diagnostics and throw failed in case of an error.
        //
        void
        apply_value_attributes (const variable*, // Optional.
                                value& lhs,
                                value&& rhs,
                                const string& attributes,
                                token_type assign_kind,
                                const path& name); // For diagnostics.

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
                        bool one = false);

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

        void
        pre_parse_directive (token&, token_type&);

        void
        pre_parse_include_line (names, location);

        description
        pre_parse_leading_description (token&, token_type&);

        description
        parse_trailing_description (token&, token_type&);

        value
        parse_variable_line (token&, token_type&);

        command_expr
        parse_command_line (token&, token_type&);

        // Ordered sequence of here-document redirects that we can expect to
        // see after the command line.
        //
        struct here_redirect
        {
          size_t expr; // Index in command_expr.
          size_t pipe; // Index in command_pipe.
          int fd;      // Redirect fd (0 - in, 1 - out, 2 - err).
        };

        struct here_doc
        {
          // Redirects that share here_doc. Most of the time we will have no
          // more than 2 (2 - for the roundtrip test cases). Doesn't refer
          // overridden redirects and thus can be empty.
          //
          small_vector<here_redirect, 2> redirects;

          string end;
          bool literal;     // Literal (single-quote).
          string modifiers;

          // Regex introducer ('\0' if not a regex, so can be used as bool).
          //
          char regex;

          // Regex global flags. Meaningful if regex != '\0'.
          //
          string regex_flags;
        };
        using here_docs = vector<here_doc>;

        pair<command_expr, here_docs>
        parse_command_expr (token&, token_type&);

        command_exit
        parse_command_exit (token&, token_type&);

        void
        parse_here_documents (token&, token_type&,
                              pair<command_expr, here_docs>&);

        struct parsed_doc
        {
          union
          {
            string str;        // Here-document literal.
            regex_lines regex; // Here-document regex.
          };

          bool re;             // True if regex.
          uint64_t end_line;   // Here-document end marker location.
          uint64_t end_column;

          parsed_doc (string, uint64_t line, uint64_t column);
          parsed_doc (regex_lines&&, uint64_t line, uint64_t column);
          parsed_doc (parsed_doc&&); // Note: move constuctible-only type.
          ~parsed_doc ();
        };

        parsed_doc
        parse_here_document (token&, token_type&,
                             const string&,
                             const string& mode,
                             char re_intro);      // '\0' if not a regex.

        // Execute. Issue diagnostics and throw failed in case of an error.
        //
      public:
        void
        execute (script& s, runner& r);

        void
        execute (scope&, script&, runner&);

      protected:
        void
        exec_scope_body ();

        // Return false if the execution of the scope should be terminated
        // with the success status (e.g., as a result of encountering the exit
        // builtin). For unsuccessful termination the failed exception should
        // be thrown.
        //
        bool
        exec_lines (lines::iterator, lines::iterator, size_t&, command_type);

        // Customization hooks.
        //
      protected:
        virtual lookup
        lookup_variable (name&&, string&&, const location&) override;

        // Number of quoted tokens since last reset. Note that this includes
        // the peeked token, if any.
        //
      protected:
        size_t
        quoted () const;

        void
        reset_quoted (token& current);

        size_t replay_quoted_;

        // Insert id into the id map checking for duplicates.
        //
      protected:
        const string&
        insert_id (string, location);

        // Set lexer pointers for both the current and the base classes.
        //
      protected:
        void
        set_lexer (lexer* l);

      protected:
        using base_parser = build2::parser;

        script* script_;

        // Pre-parse state.
        //
        using id_map = std::unordered_map<string, location>;
        using include_set = std::set<path>;

        group* group_;
        id_map* id_map_;
        include_set* include_set_; // Testscripts already included in this
                                   // scope. Must be absolute and normalized.
        lexer* lexer_;
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
