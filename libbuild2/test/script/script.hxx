// file      : libbuild2/test/script/script.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_SCRIPT_SCRIPT_HXX
#define LIBBUILD2_TEST_SCRIPT_SCRIPT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/variable.hxx>

#include <libbuild2/script/script.hxx>

#include <libbuild2/test/target.hxx>

namespace build2
{
  namespace test
  {
    namespace script
    {
      using build2::script::line;
      using build2::script::line_type;
      using build2::script::lines;
      using build2::script::redirect;
      using build2::script::redirect_type;
      using build2::script::command;
      using build2::script::expr_term;
      using build2::script::command_expr;
      using build2::script::iteration_index;
      using build2::script::environment_vars;
      using build2::script::deadline;
      using build2::script::timeout;
      using build2::script::pipe_command;
      using build2::script::command_function;

      class parser; // Required by VC for 'friend class parser' declaration.

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

      class scope_base // Make sure certain things are initialized early.
      {
      public:
        script& root; // Self for the root (script) scope.

        // Note that if we pass the variable name as a string, then it will
        // be looked up in the wrong pool.
        //
        variable_map vars;

      protected:
        scope_base (script&);

        const dir_path*
        wd_path () const;

        const target_triplet&
        test_tt () const;
      };

      enum class scope_state {unknown, passed, failed};

      class scope: public scope_base, public build2::script::environment
      {
      public:
        scope* const parent; // NULL for the root (script) scope.

        // The chain of if-else scope alternatives. See also if_cond_ below.
        //
        unique_ptr<scope> if_chain;

        const path& id_path;     // Id path ($@, relative in POSIX form).

        optional<description> desc;

        scope_state state = scope_state::unknown;

        // The command expression execution nesting level. Can be maintained
        // by the runner to, for example, only perform some housekeeping on
        // the topmost level (add the test id to the diagnostics, etc).
        //
        // Note that the command expression execution can be nested, so that
        // the outer expression execution is not completed before all the
        // inner expressions are executed. As for example in:
        //
        // echo 'a b' | for x
        //   echo 'c d' | for y
        //     test $x $y
        //   end
        // end
        //
        size_t exec_level = 0;

        // Test program paths.
        //
        // Currently always contains a single element (see test_program() for
        // details). While in the future there can be more of them, the zero
        // index will always refer to the test variable value and can
        // potentially be NULL (see reset_special() for details).
        //
        small_vector<const path*, 1> test_programs;

        virtual void
        set_variable (string name,
                      names&&,
                      const string& attrs,
                      const location&) override;

        // Merge the command execution environment variable (un)sets from this
        // and outer scopes.
        //
        virtual const environment_vars&
        exported_variables (environment_vars& storage) override;

        // Noop since the temporary directory is a working directory and so
        // is created before the scope commands execution.
        //
        virtual void
        create_temp_dir () override {assert (false);};

        // Return true if this is a test program path.
        //
        // Note that currently the test program is only specified via the test
        // variable ($0 effectively). In the future we may invent some other
        // means of marking a program as a test (builtin, etc).
        //
        bool
        test_program (const path&);

        // Variables.
        //
      public:
        // Lookup the variable starting from this scope, continuing with outer
        // scopes, then the target being tested, then the testscript target,
        // and then outer buildfile scopes (including testscript-type/pattern
        // specific).
        //
        using lookup_type = build2::lookup;

        lookup_type
        lookup (const variable&) const;

        // As above but only look for buildfile variables. If target_only is
        // false then also look in scopes of the test target (this should only
        // be done if the variable's visibility is target).
        //
        lookup_type
        lookup_in_buildfile (const string&, bool target_only = true) const;

        // Return a value suitable for assignment. If the variable does not
        // exist in this scope's variable map, then a new one with the NULL
        // value is added and returned. Otherwise the existing value is
        // returned.
        //
        value&
        assign (const variable& var) {return vars.assign (var);}

        // Return a value suitable for append/prepend. If the variable does
        // not exist in this scope's variable map, then outer scopes are
        // searched for the same variable. If found then a new variable with
        // the found value is added to this scope and returned. Otherwise this
        // function proceeds as assign() above.
        //
        value&
        append (const variable&);

        // Reset special $*, $N variables based on the test.* values.
        //
        void
        reset_special ();

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
        group (const string& id, group& p): scope (id, &p, p.root) {}

      public:
        vector<unique_ptr<scope>> scopes;

        // The test group execution deadline and the individual test timeout.
        //
        optional<deadline> group_deadline;
        optional<timeout>  test_timeout;

        // Parse the argument having the '[<group-timeout>]/[<test-timeout>]'
        // form, where the values are expressed in seconds and either of them
        // (but not both) can be omitted, and set the group deadline and test
        // timeout respectively, if specified. Reset them to nullopt on zero.
        //
        virtual void
        set_timeout (const string&, bool success, const location&) override;

        // Return the nearest of the own deadline and the enclosing groups
        // deadlines.
        //
        virtual optional<deadline>
        effective_deadline () override;

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

      public:
        // The whole test and the remaining test fragment execution deadlines.
        //
        // The former is based on the minimum of the test timeouts set for the
        // enclosing scopes and is calculated on the first deadline() call.
        // The later is set by set_timeout() from the timeout builtin call
        // during the test execution.
        //
        optional<optional<deadline>> test_deadline; // calculated<specified<>>
        optional<deadline>           fragment_deadline;

        // Parse the specified in seconds timeout and set the remaining test
        // fragment execution deadline. Reset it to nullopt on zero.
        //
        virtual void
        set_timeout (const string&, bool success, const location&) override;

        // Return the nearest of the test and fragment execution deadlines,
        // calculating the former on the first call.
        //
        virtual optional<deadline>
        effective_deadline () override;

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

        // Used to compose a test command.
        //
        // Changing any of their values requires resetting the $* and $N
        // special aliases.
        //
        const variable& test_var;      // test
        const variable& options_var;   // test.options
        const variable& arguments_var; // test.arguments
        const variable& redirects_var; // test.redirects
        const variable& cleanups_var;  // test.cleanups

        bool
        test_command_var (const string& name) const
        {
          return name == test_var.name      ||
                 name == options_var.name   ||
                 name == arguments_var.name ||
                 name == redirects_var.name ||
                 name == cleanups_var.name;
        }

        const variable& wd_var;       // $~
        const variable& id_var;       // $@
        const variable& cmd_var;      // $*
        const variable* cmdN_var[10]; // $N
      };

      class script: public script_base, public group
      {
      public:
        // The test operation deadline and the individual test timeout (see
        // the config.test.timeout variable for details).
        //
        optional<deadline> operation_deadline;
        optional<timeout>  test_timeout;

      public:
        script (const target& test_target,
                const testscript& script_target,
                const dir_path& root_wd);

        script (script&&) = delete;
        script (const script&) = delete;
        script& operator= (script&&) = delete;
        script& operator= (const script&) = delete;

        // Return the nearest of the test operation and group execution
        // deadlines.
        //
        virtual optional<deadline>
        effective_deadline () override;

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

        set<path_name_value, compare_paths> paths_;
      };
    }
  }
}

#endif // LIBBUILD2_TEST_SCRIPT_SCRIPT_HXX
