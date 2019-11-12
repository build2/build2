// file      : libbuild2/test/script/script.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_SCRIPT_SCRIPT_HXX
#define LIBBUILD2_TEST_SCRIPT_SCRIPT_HXX

#include <set>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/variable.hxx>

#include <libbuild2/test/target.hxx>

#include <libbuild2/test/script/token.hxx> // replay_tokens

namespace build2
{
  namespace test
  {
    namespace script
    {
      class parser; // Required by VC for 'friend class parser' declaration.

      // Pre-parse representation.
      //

      enum class line_type
      {
        var,
        cmd,
        cmd_if,
        cmd_ifn,
        cmd_elif,
        cmd_elifn,
        cmd_else,
        cmd_end
      };

      ostream&
      operator<< (ostream&, line_type);

      struct line
      {
        line_type type;
        replay_tokens tokens;

        union
        {
          const variable* var; // Pre-entered for line_type::var.
        };
      };

      // Most of the time we will have just one line (test command).
      //
      using lines = small_vector<line, 1>;

      // Parse object model.
      //

      // redirect
      //
      enum class redirect_type
      {
        none,
        pass,
        null,
        trace,
        merge,
        here_str_literal,
        here_str_regex,
        here_doc_literal,
        here_doc_regex,
        here_doc_ref,     // Reference to here_doc literal or regex.
        file,
      };

      // Pre-parsed (but not instantiated) regex lines. The idea here is that
      // we should be able to re-create their (more or less) exact text
      // representation for diagnostics but also instantiate without any
      // re-parsing.
      //
      struct regex_line
      {
        // If regex is true, then value is the regex expression. Otherwise, it
        // is a literal. Note that special characters can be present in both
        // cases. For example, //+ is a regex, while /+ is a literal, both
        // with '+' as a special character. Flags are only valid for regex.
        // Literals falls apart into textual (has no special characters) and
        // special (has just special characters instead) ones. For example
        // foo is a textual literal, while /.+ is a special one. Note that
        // literal must not have value and special both non-empty.
        //
        bool regex;

        string value;
        string flags;
        string special;

        uint64_t line;
        uint64_t column;

        // Create regex with optional special characters.
        //
        regex_line (uint64_t l, uint64_t c,
                    string v, string f, string s = string ())
            : regex (true),
              value (move (v)),
              flags (move (f)),
              special (move (s)),
              line (l),
              column (c) {}

        // Create a literal, either text or special.
        //
        regex_line (uint64_t l, uint64_t c, string v, bool s)
            : regex (false),
              value (s ? string () : move (v)),
              special (s ? move (v) : string ()),
              line (l),
              column (c) {}
      };

      struct regex_lines
      {
        char intro;   // Introducer character.
        string flags; // Global flags (here-document).

        small_vector<regex_line, 8> lines;
      };

      // Output file redirect mode.
      //
      enum class redirect_fmode
      {
        compare,
        overwrite,
        append
      };

      struct redirect
      {
        redirect_type type;

        struct file_type
        {
          using path_type = build2::path;
          path_type path;
          redirect_fmode mode; // Meaningless for input redirect.
        };

        union
        {
          int         fd;    // Merge-to descriptor.
          string      str;   // Note: with trailing newline, if requested.
          regex_lines regex; // Note: with trailing blank, if requested.
          file_type   file;
          reference_wrapper<const redirect> ref; // Note: no chains.
        };

        string modifiers;   // Redirect modifiers.
        string end;         // Here-document end marker (no regex intro/flags).
        uint64_t end_line;  // Here-document end marker location.
        uint64_t end_column;

        // Create redirect of a type other than reference.
        //
        explicit
        redirect (redirect_type = redirect_type::none);

        // Create redirect of the reference type.
        //
        redirect (redirect_type t, const redirect& r)
            : type (redirect_type::here_doc_ref), ref (r)
        {
          // There is no support (and need) for reference chains.
          //
          assert (t == redirect_type::here_doc_ref &&
                  r.type != redirect_type::here_doc_ref);
        }

        // Move constuctible/assignable-only type.
        //
        redirect (redirect&&);
        redirect& operator= (redirect&&);

        ~redirect ();

        const redirect&
        effective () const noexcept
        {
          return type == redirect_type::here_doc_ref ? ref.get () : *this;
        }
      };

      // cleanup
      //
      enum class cleanup_type
      {
        always, // &foo  - cleanup, fail if does not exist.
        maybe,  // &?foo - cleanup, ignore if does not exist.
        never   // &!foo - don’t cleanup, ignore if doesn’t exist.
      };

      // File or directory to be automatically cleaned up at the end of the
      // scope. If the path ends with a trailing slash, then it is assumed to
      // be a directory, otherwise -- a file. A directory that is about to be
      // cleaned up must be empty.
      //
      // The last component in the path may contain a wildcard that have the
      // following semantics:
      //
      // dir/*   - remove all immediate files
      // dir/*/  - remove all immediate sub-directories (must be empty)
      // dir/**  - remove all files recursively
      // dir/**/ - remove all sub-directories recursively (must be empty)
      // dir/*** - remove directory dir with all files and sub-directories
      //           recursively
      //
      struct cleanup
      {
        cleanup_type type;
        build2::path path;
      };
      using cleanups = vector<cleanup>;

      // command_exit
      //
      enum class exit_comparison {eq, ne};

      struct command_exit
      {
        // C/C++ don't apply constraints on program exit code other than it
        // being of type int.
        //
        // POSIX specifies that only the least significant 8 bits shall be
        // available from wait() and waitpid(); the full value shall be
        // available from waitid() (read more at _Exit, _exit Open Group
        // spec).
        //
        // While the Linux man page for waitid() doesn't mention any
        // deviations from the standard, the FreeBSD implementation (as of
        // version 11.0) only returns 8 bits like the other wait*() calls.
        //
        // Windows supports 32-bit exit codes.
        //
        // Note that in shells some exit values can have special meaning so
        // using them can be a source of confusion. For bash values in the
        // [126, 255] range are such a special ones (see Appendix E, "Exit
        // Codes With Special Meanings" in the Advanced Bash-Scripting Guide).
        //
        exit_comparison comparison;
        uint8_t code;
      };

      // command
      //
      struct command
      {
        path program;
        strings arguments;

        redirect in;
        redirect out;
        redirect err;

        script::cleanups cleanups;

        command_exit exit {exit_comparison::eq, 0};
      };

      enum class command_to_stream: uint16_t
      {
        header   = 0x01,
        here_doc = 0x02,              // Note: printed on a new line.
        all      = header | here_doc
      };

      void
      to_stream (ostream&, const command&, command_to_stream);

      ostream&
      operator<< (ostream&, const command&);

      // command_pipe
      //
      using command_pipe = vector<command>;

      void
      to_stream (ostream&, const command_pipe&, command_to_stream);

      ostream&
      operator<< (ostream&, const command_pipe&);

      // command_expr
      //
      enum class expr_operator {log_or, log_and};

      struct expr_term
      {
        expr_operator op;  // OR-ed to an implied false for the first term.
        command_pipe pipe;
      };

      using command_expr = vector<expr_term>;

      void
      to_stream (ostream&, const command_expr&, command_to_stream);

      ostream&
      operator<< (ostream&, const command_expr&);

      // command_type
      //
      enum class command_type {test, setup, teardown};

      // description
      //
      struct description
      {
        string id;
        string summary;
        string details;

        bool
        empty () const
        {
          return id.empty () && summary.empty () && details.empty ();
        }
      };

      // scope
      //
      class script;

      enum class scope_state {unknown, passed, failed};

      class scope
      {
      public:
        scope* const parent; // NULL for the root (script) scope.
        script&      root;   // Self for the root (script) scope.

        // The chain of if-else scope alternatives. See also if_cond_ below.
        //
        unique_ptr<scope> if_chain;

        // Note that if we pass the variable name as a string, then it will
        // be looked up in the wrong pool.
        //
        variable_map vars;

        const path& id_path;     // Id path ($@, relative in POSIX form).
        const dir_path& wd_path; // Working dir ($~, absolute and normalized).

        optional<description> desc;

        scope_state state = scope_state::unknown;
        test::script::cleanups cleanups;
        paths special_cleanups;

        // Variables.
        //
      public:
        // Lookup the variable starting from this scope, continuing with outer
        // scopes, then the target being tested, then the testscript target,
        // and then outer buildfile scopes (including testscript-type/pattern
        // specific).
        //
        lookup
        find (const variable&) const;

        // As above but only look for buildfile variables. If target_only is
        // false then also look in scopes of the test target (this should only
        // be done if the variable's visibility is target).
        //
        lookup
        find_in_buildfile (const string&, bool target_only = true) const;

        // Return a value suitable for assignment. If the variable does not
        // exist in this scope's map, then a new one with the NULL value is
        // added and returned. Otherwise the existing value is returned.
        //
        value&
        assign (const variable& var) {return vars.assign (var);}

        // Return a value suitable for append/prepend. If the variable does
        // not exist in this scope's map, then outer scopes are searched for
        // the same variable. If found then a new variable with the found
        // value is added to this scope and returned. Otherwise this function
        // proceeds as assign() above.
        //
        value&
        append (const variable&);

        // Reset special $*, $N variables based on the test.* values.
        //
        void
        reset_special ();

        // Cleanup.
        //
      public:
        // Register a cleanup. If the cleanup is explicit, then override the
        // cleanup type if this path is already registered. Ignore implicit
        // registration of a path outside script working directory.
        //
        void
        clean (cleanup, bool implicit);

        // Register cleanup of a special file. Such files are created to
        // maintain testscript machinery and must be removed first, not to
        // interfere with the user-defined wildcard cleanups.
        //
        void
        clean_special (path p);

      public:
        virtual
        ~scope () = default;

      protected:
        scope (const string& id, scope* parent, script& root);

        // Pre-parse data.
        //
      public:
        virtual bool
        empty () const = 0;

      protected:
        friend class parser;

        location start_loc_;
        location end_loc_;

        optional<line> if_cond_;
      };

      // group
      //
      class group: public scope
      {
      public:
        vector<unique_ptr<scope>> scopes;

      public:
        group (const string& id, group& p): scope (id, &p, p.root) {}

      protected:
        group (const string& id, script& r): scope (id, nullptr, r) {}

        // Pre-parse data.
        //
      public:
        virtual bool
        empty () const override
        {
          return
            !if_cond_ && // The condition expression can have side-effects.
            setup_.empty () &&
            tdown_.empty () &&
            find_if (scopes.begin (), scopes.end (),
                     [] (const unique_ptr<scope>& s)
                     {
                       return !s->empty ();
                     }) == scopes.end ();
        }

      private:
        friend class parser;

        lines setup_;
        lines tdown_;
      };

      // test
      //
      class test: public scope
      {
      public:
        test (const string& id, group& p): scope (id, &p, p.root) {}

        // Pre-parse data.
        //
      public:
        virtual bool
        empty () const override
        {
          return tests_.empty ();
        }

      private:
        friend class parser;

        lines tests_;
      };

      // script
      //
      class script_base // Make sure certain things are initialized early.
      {
      protected:
        script_base (const target& test_target,
                     const testscript& script_target);

      public:
        const target&        test_target;   // Target we are testing.
        const build2::scope& target_scope;  // Base scope of test target.
        const testscript&    script_target; // Target of the testscript file.

      public:
        variable_pool var_pool;
        mutable shared_mutex var_pool_mutex;

        const variable& test_var;      // test
        const variable& options_var;   // test.options
        const variable& arguments_var; // test.arguments
        const variable& redirects_var; // test.redirects
        const variable& cleanups_var;  // test.cleanups

        const variable& wd_var;       // $~
        const variable& id_var;       // $@
        const variable& cmd_var;      // $*
        const variable* cmdN_var[10]; // $N
      };

      class script: public script_base, public group
      {
      public:
        script (const target& test_target,
                const testscript& script_target,
                const dir_path& root_wd);

        script (script&&) = delete;
        script (const script&) = delete;
        script& operator= (script&&) = delete;
        script& operator= (const script&) = delete;

        // Pre-parse data.
        //
      private:
        friend class parser;

        // Testscript file paths. Specifically, replay_token::file points to
        // these path names.
        //
        struct compare_paths
        {
          bool operator() (const path_name_value& x,
                           const path_name_value& y) const
          {
            // Note that these path names are always paths, so we compare them
            // as paths.
            //
            return x.path < y.path;
          }
        };

        std::set<path_name_value, compare_paths> paths_;
      };
    }
  }
}

#include <libbuild2/test/script/script.ixx>

#endif // LIBBUILD2_TEST_SCRIPT_SCRIPT_HXX
