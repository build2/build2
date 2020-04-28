// file      : libbuild2/script/parser.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCRIPT_PARSER_HXX
#define LIBBUILD2_SCRIPT_PARSER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/parser.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/script/token.hxx>
#include <libbuild2/script/lexer.hxx>  // redirect_aliases
#include <libbuild2/script/script.hxx>

namespace build2
{
  namespace script
  {
    class  lexer;
    struct lexer_mode;

    class parser: protected build2::parser
    {
    public:
      parser (context& c): build2::parser (c) {}

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
                              const path_name&); // For diagnostics.

      using build2::parser::apply_value_attributes;

      // Commonly used parsing functions. Issue diagnostics and throw failed
      // in case of an error.
      //
      // Usually (but not always) parse functions receive the token/type
      // from which it should start consuming and in return the token/type
      // should contain the first token that has not been consumed.
      //
      // Functions that are called parse_*() rather than pre_parse_*() can be
      // used for both stages.
      //
    protected:
      value
      parse_variable_line (token&, token_type&);

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
        // more than 2 (2 - for the roundtrip cases). Doesn't refer overridden
        // redirects and thus can be empty.
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
      parse_command_expr (token&, token_type&, const redirect_aliases&);

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

      // Start pre-parsing a script line returning its type, detected based on
      // the first two tokens. Use the specified lexer mode to peek the second
      // token.
      //
      line_type
      pre_parse_line_start (token&, token_type&, lexer_mode);

      // Execute.
      //
    protected:
      // Return false if the execution of the script should be terminated with
      // the success status (e.g., as a result of encountering the exit
      // builtin). For unsuccessful termination the failed exception is thrown.
      //
      using exec_set_function = void (const variable&,
                                      token&, token_type&,
                                      const location&);

      using exec_cmd_function = void (token&, token_type&,
                                      size_t li,
                                      bool single,
                                      const location&);

      using exec_if_function  = bool (token&, token_type&,
                                      size_t li,
                                      const location&);

      // If a parser implementation doesn't pre-enter variables into a pool
      // during the pre-parsing phase, then they are entered during the
      // execution phase and so the variable pool must be provided. Note that
      // in this case the variable pool insertions are not MT-safe.
      //
      bool
      exec_lines (lines::const_iterator b, lines::const_iterator e,
                  const function<exec_set_function>&,
                  const function<exec_cmd_function>&,
                  const function<exec_if_function>&,
                  size_t& li,
                  variable_pool* = nullptr);

      // Set lexer pointers for both the current and the base classes.
      //
    protected:
      void
      set_lexer (lexer*);

      // Number of quoted tokens since last reset. Note that this includes
      // the peeked token, if any.
      //
    protected:
      size_t
      quoted () const;

      void
      reset_quoted (token& current);

      size_t replay_quoted_;

    protected:
      lexer* lexer_ = nullptr;
    };
  }
}

#endif // LIBBUILD2_SCRIPT_PARSER_HXX
