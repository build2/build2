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

      // Return true if a command line element needs to be re-lexed.
      //
      // Specifically, it needs to be re-lexed if it contains any of the
      // special characters (|<>&), quotes ("') or effective escape sequences
      // (\", \', \\).
      //
      static bool
      need_cmdline_relex (const string&);

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

      struct parse_command_expr_result
      {
        command_expr expr; // Single pipe for the for-loop.
        here_docs    docs;
        bool         for_loop = false;

        parse_command_expr_result () = default;

        parse_command_expr_result (command_expr&& e,
                                   here_docs&& h,
                                   bool f)
            : expr (move (e)), docs (move (h)), for_loop (f) {}
      };

      // Pass the first special command program name (token_type::word) if it
      // is already pre-parsed.
      //
      parse_command_expr_result
      parse_command_expr (token&, token_type&,
                          const redirect_aliases&,
                          optional<token>&& program = nullopt);

      command_exit
      parse_command_exit (token&, token_type&);

      void
      parse_here_documents (token&, token_type&,
                            parse_command_expr_result&);

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
        parsed_doc (parsed_doc&&) noexcept; // Note: move constuctible-only type.
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
      // Always return the cmd_for_stream line type for the for-loop. Note
      // that the for-loop form cannot be detected easily, based on the first
      // two tokens. Also note that the detection can be specific for the
      // script implementation (custom lexing mode, special variables, etc).
      //
      line_type
      pre_parse_line_start (token&, token_type&, lexer_mode);

      // Parse the env pseudo-builtin arguments up to the program name. Return
      // the program execution timeout and its success flag, CWD, the list of
      // the variables that should be unset ("name") and/or set ("name=value")
      // in the command environment, and the token/type that starts the
      // program name. Note that the variable unsets come first, if present.
      //
      struct parsed_env
      {
        optional<duration> timeout;
        bool               timeout_success = false;
        optional<dir_path> cwd;
        environment_vars   variables;
      };

      parsed_env
      parse_env_builtin (token&, token_type&);

      // Execute.
      //
    protected:
      // Return false if the execution of the script should be terminated with
      // the success status (e.g., as a result of encountering the exit
      // builtin). For unsuccessful termination the failed exception is
      // thrown.
      //
      using exec_set_function = void (const variable&,
                                      token&, token_type&,
                                      const location&);

      using exec_cmd_function = void (token&, token_type&,
                                      const iteration_index*, size_t li,
                                      bool single,
                                      const function<command_function>&,
                                      const location&);

      using exec_cond_function  = bool (token&, token_type&,
                                        const iteration_index*, size_t li,
                                        const location&);

      using exec_for_function = void (const variable&,
                                      value&&,
                                      const attributes& value_attrs,
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
                  const function<exec_cond_function>&,
                  const function<exec_for_function>&,
                  const iteration_index*, size_t& li,
                  variable_pool* = nullptr);

      // Customization hooks.
      //
    protected:
      // Parse the command's leading name chunk. The argument first is true if
      // this is the first command in the line. The argument env is true if
      // the command is executed via the env pseudo-builtin.
      //
      // During the execution phase try to parse and translate the leading
      // names into the process path and return nullopt if choose not to do
      // so, leaving it to the parser to handle. Also return in the last
      // two arguments uninterpreted names, if any.
      //
      // The default implementation always returns nullopt. The derived parser
      // can provide an override that can, for example, handle process path
      // values, executable targets, etc.
      //
      // Note that normally it makes sense to leave simple unpaired names for
      // the parser to handle, unless there is a good reason not to (e.g.,
      // it's a special builtin or some such). Such names may contain
      // something that requires re-lexing, for example `foo|bar`, which won't
      // be easy to translate but which are handled by the parser.
      //
      // Note that the chunk could be of the special cmdline type in which
      // case the names may need to be "preprocessed" (at least unquoted or
      // potentially fully re-lexed) before being analyzed/consumed. Note also
      // that in this case any names left unconsumed must remain of the
      // cmdline type.
      //
      //
      // During the pre-parsing phase the returned process path and names
      // (that must still be parsed) are discarded. The main purpose of the
      // call is to allow implementations to perform static script analysis,
      // recognize and execute certain directives, or some such.
      //
      virtual optional<process_path>
      parse_program (token&, token_type&,
                     bool first, bool env,
                     names&, parse_names_result&);

      // Set lexer pointers for both the current and the base classes.
      //
    protected:
      void
      set_lexer (lexer*);

      // Number of quoted tokens since last reset. Note that this includes the
      // peeked token, if any.
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
