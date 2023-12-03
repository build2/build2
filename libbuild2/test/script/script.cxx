// file      : libbuild2/test/script/script.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/script/script.hxx>

#include <sstream>

#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>

#include <libbuild2/script/timeout.hxx>

#include <libbuild2/test/common.hxx>        // operation_deadline(),
                                            // test_timeout()
#include <libbuild2/test/script/parser.hxx>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using build2::script::to_deadline;
      using build2::script::to_timeout;

      // scope_base
      //
      scope_base::
      scope_base (script& s)
          : root (s),
            vars (s.test_target.ctx, false /* shared */) // Note: managed.
      {
        vars.assign (root.wd_var) = dir_path ();
      }

      const dir_path* scope_base::
      wd_path () const
      {
        return &cast<dir_path> (vars[root.wd_var]);
      }

      const target_triplet& scope_base::
      test_tt () const
      {
        if (auto r =
            cast_null<target_triplet> (root.test_target["test.target"]))
          return *r;

        // We set it to default value in init() so it can only be NULL if the
        // user resets it.
        //
        fail << "invalid test.target value" << endf;
      }

      // scope
      //
      static const optional<string> wd_name ("test working directory");
      static const optional<string> sd_name ("working directory");

      scope::
      scope (const string& id, scope* p, script& r)
          : scope_base (r),
            //
            // Note that root.work_dir is not yet constructed if we are
            // creating the root scope (p is NULL). Also note that
            // root.test_target is always constructed to date.
            //
            environment (root.test_target.ctx,
                         test_tt (),
                         dir_name_view (wd_path (), &wd_name),
                         dir_name_view (
                           p != nullptr ? root.work_dir.path : wd_path (),
                           &sd_name),
                         *wd_path (), true /* temp_dir_keep */,
                         redirect (redirect_type::none),
                         redirect (redirect_type::none),
                         redirect (redirect_type::none)),
            parent (p),
            id_path (cast<path> (assign (root.id_var) = path ()))
      {
        // Construct the id_path as a string to ensure POSIX form. In fact,
        // the only reason we keep it as a path is to be able to easily get id
        // by calling leaf().
        //
        {
          string s (p != nullptr ? p->id_path.string () : string ());

          if (!s.empty () && !id.empty ())
            s += '/';

          s += id;
          const_cast<path&> (id_path) = path (move (s));
        }

        // Calculate the working directory path unless this is the root scope
        // (handled in an ad hoc way).
        //
        if (p != nullptr)
          const_cast<dir_path&> (*work_dir.path) =
            dir_path (*p->work_dir.path) /= id;
      }

      bool scope::
      test_program (const path& p)
      {
        assert (!test_programs.empty ());

        return find_if (test_programs.begin (), test_programs.end (),
                        [&p] (const path* tp)
                        {
                          return tp != nullptr ? *tp == p : false;
                        }) != test_programs.end ();
      }

      void scope::
      set_variable (string nm,
                    names&& val,
                    const string& attrs,
                    const location& ll)
      {
        // Check if we are trying to modify any of the special variables.
        //
        if (parser::special_variable (nm))
          fail (ll) << "attempt to set '" << nm << "' variable directly";

        // Set the variable value and attributes. Note that we need to aquire
        // unique lock before potentially changing the script's variable
        // pool. The obtained variable reference can safelly be used with no
        // locking as the variable pool is an associative container
        // (underneath) and we are only adding new variables into it.
        //
        ulock ul (root.var_pool_mutex);
        const variable& var (root.var_pool.insert (move (nm)));
        ul.unlock ();

        value& lhs (assign (var));

        // If there are no attributes specified then the variable assignment
        // is straightforward. Otherwise we will use the build2 parser helper
        // function.
        //
        if (attrs.empty ())
          lhs.assign (move (val), &var);
        else
        {
          // If there is an error in the attributes string, our diagnostics
          // will look like this:
          //
          // <attributes>:1:1 error: unknown value attribute x
          //   testscript:10:1 info: while parsing attributes '[x]'
          //
          // Note that the attributes parsing error is the only reason for a
          // failure.
          //
          auto df = make_diag_frame (
            [attrs, &ll](const diag_record& dr)
            {
              dr << info (ll) << "while parsing attributes '" << attrs << "'";
            });

          parser p (context);
          p.apply_value_attributes (&var,
                                    lhs,
                                    value (move (val)),
                                    attrs,
                                    token_type::assign,
                                    path_name ("<attributes>"));
        }

        if (root.test_command_var (var.name))
          reset_special ();
      }

      const environment_vars& scope::
      exported_variables (environment_vars& storage)
      {
        return parent != nullptr
               ? parent->merge_exported_variables (exported_vars, storage)
               : exported_vars;
      }

      // script_base
      //
      script_base::
      script_base (const target& tt, const testscript& st)
          : test_target (tt),
            target_scope (tt.base_scope ()),
            script_target (st),
            // Enter the test.* variables with the same variable types as in
            // buildfiles except for test: while in buildfiles it can be a
            // target name, in testscripts it should be resolved to a path.
            //
            // Note: entering in a custom variable pool.
            //
            test_var      (var_pool.insert<path> ("test")),
            options_var   (var_pool.insert<strings> ("test.options")),
            arguments_var (var_pool.insert<strings> ("test.arguments")),
            redirects_var (var_pool.insert<cmdline> ("test.redirects")),
            cleanups_var  (var_pool.insert<cmdline> ("test.cleanups")),

            wd_var (var_pool.insert<dir_path> ("~")),
            id_var (var_pool.insert<path> ("@")),
            cmd_var (var_pool.insert<cmdline> ("*")),
            cmdN_var {
              &var_pool.insert<path> ("0"),
              &var_pool.insert<string> ("1"),
              &var_pool.insert<string> ("2"),
              &var_pool.insert<string> ("3"),
              &var_pool.insert<string> ("4"),
              &var_pool.insert<string> ("5"),
              &var_pool.insert<string> ("6"),
              &var_pool.insert<string> ("7"),
              &var_pool.insert<string> ("8"),
              &var_pool.insert<string> ("9")} {}

      // script
      //
      script::
      script (const target& tt, const testscript& st, const dir_path& rwd)
          : script_base (tt, st),
            group (st.name == "testscript" ? string () : st.name, *this),
            operation_deadline (
              to_deadline (build2::test::operation_deadline (tt),
                           false /* success */)),
            test_timeout (to_timeout (build2::test::test_timeout (tt),
                                      false /* success */))
      {
        // Set the script working dir ($~) to $out_base/test/<id> (id_path
        // for root is just the id which is empty if st is 'testscript').
        //
        const_cast<dir_path&> (*work_dir.path) =
          dir_path (rwd) /= id_path.string ();

        // Set the test variable at the script level. We do it even if it's
        // set in the buildfile since they use different types.
        //
        {
          value& v (assign (test_var));

          // Note that the test variable's visibility is target.
          //
          auto l (lookup_in_buildfile ("test", false));

          // Note that we have similar code for simple tests.
          //
          const target* t (nullptr);

          if (l.defined ())
          {
            const name* n (cast_null<name> (l));

            if (n == nullptr)
              v = nullptr;
            else if (n->empty ())
              v = path ();
            else if (n->simple ())
            {
              // Ignore the special 'true' value.
              //
              if (n->value != "true")
                v = path (n->value);
              else
                t = &tt;
            }
            else if (n->directory ())
              v = path (n->dir);
            else
            {
              // Must be a target name. Could be from src (e.g., a script).
              //
              // @@ OUT: what if this is a @-qualified pair of names?
              //
              t = search_existing (*n, target_scope);

              if (t == nullptr)
                fail << "unknown target '" << *n << "' in test variable";
            }
          }
          else
            // By default we set it to the test target's path.
            //
            t = &tt;

          // If this is a path-based target, then we use the path. If this
          // is an alias target (e.g., dir{}), then we use the directory
          // path. Otherwise, we leave it NULL expecting the testscript to
          // set it to something appropriate, if used.
          //
          if (t != nullptr)
          {
            if (auto* pt = t->is_a<path_target> ())
            {
              // Do some sanity checks: the target better be up-to-date with
              // an assigned path.
              //
              v = pt->path ();

              if (v.empty ())
                fail << "target " << *pt << " specified in the test variable "
                     << "is out of date" <<
                  info << "consider specifying it as a prerequisite of " << tt;
            }
            else if (t->is_a<alias> ())
              v = path (t->dir);
            else if (t != &tt)
              fail << "target " << *t << " specified in the test variable "
                   << "is not path-based";
          }
        }

        // Reserve the entry for the test program specified via the test
        // variable. Note that the value will be assigned by the below
        // reset_special() call.
        //
        test_programs.push_back (nullptr);

        // Set the special $*, $N variables.
        //
        reset_special ();
      }

      optional<deadline> script::
      effective_deadline ()
      {
        return earlier (operation_deadline, group_deadline);
      }

      // scope
      //
      lookup scope::
      lookup (const variable& var) const
      {
        // Search script scopes until we hit the root.
        //
        const scope* s (this);

        do
        {
          auto p (s->vars.lookup (var));
          if (p.first != nullptr)
            return lookup_type (*p.first, p.second, s->vars);
        }
        while ((s->parent != nullptr ? (s = s->parent) : nullptr) != nullptr);

        return lookup_in_buildfile (var.name);
      }

      lookup scope::
      lookup_in_buildfile (const string& n, bool target_only) const
      {
        // Switch to the corresponding buildfile variable. Note that we don't
        // want to insert a new variable into the pool (we might be running
        // in parallel). Plus, if there is no such variable, then we cannot
        // possibly find any value.
        //
        const variable* pvar (root.target_scope.var_pool ().find (n));

        if (pvar == nullptr)
          return lookup_type ();

        const variable& var (*pvar);

        // First check the target we are testing.
        //
        {
          // Note that we skip applying the override if we did not find any
          // value. In this case, presumably the override also affects the
          // script target and we will pick it up there. A bit fuzzy.
          //
          auto p (root.test_target.lookup_original (var, target_only));

          if (p.first)
          {
            if (var.overrides != nullptr)
              p = root.target_scope.lookup_override (var, move (p), true);

            return p.first;
          }
        }

        // Then the script target followed by the scopes it is in. Note that
        // while unlikely it is possible the test and script targets will be
        // in different scopes which brings the question of which scopes we
        // should search.
        //
        return root.script_target[var];
      }

      value& scope::
      append (const variable& var)
      {
        auto l (lookup (var));

        if (l.defined () && l.belongs (*this)) // Existing var in this scope.
          return vars.modify (l);

        value& r (assign (var)); // NULL.

        if (l.defined ())
          r = *l; // Copy value (and type) from the outer scope.

        return r;
      }

      void scope::
      reset_special ()
      {
        // First assemble the $* value and save the test variable value into
        // the test program set.
        //
        cmdline s;

        auto append = [&s] (const strings& vs)
        {
          for (const string& v: vs)
            s.push_back (name (v)); // Simple name.
        };

        // If the test variable can't be looked up for any reason (is NULL,
        // etc), then keep $* empty.
        //
        if (auto l = lookup (root.test_var))
        {
          const path& p (cast<path> (l));
          s.push_back (name (p.representation ()));

          test_programs[0] = &p;

          if (auto l = lookup (root.options_var))
            append (cast<strings> (l));

          if (auto l = lookup (root.arguments_var))
            append (cast<strings> (l));
        }
        else
          test_programs[0] = nullptr;

        // Keep redirects/cleanups out of $N.
        //
        size_t n (s.size ());

        if (auto l = lookup (root.redirects_var))
        {
          const auto& v (cast<cmdline> (l));
          s.insert (s.end (), v.begin (), v.end ());
        }

        if (auto l = lookup (root.cleanups_var))
        {
          const auto& v (cast<cmdline> (l));
          s.insert (s.end (), v.begin (), v.end ());
        }

        // Set the $N values if present.
        //
        for (size_t i (0); i <= 9; ++i)
        {
          value& v (assign (*root.cmdN_var[i]));

          if (i < n)
          {
            if (i == 0)
              v = path (s[i].value);
            else
              v = s[i].value;
          }
          else
            v = nullptr; // Clear any old values.
        }

        // Set $*.
        //
        // We need to effective-quote the $test $test.options, $test.arguments
        // part of it since they will be re-lexed. See the Testscript manual
        // for details on quoting semantics. In particular, we cannot escape
        // the special character (|<>&) so we have to rely on quoting. We can
        // use single-quoting for everything except if the value contains a
        // single quote. In which case we should probably just do separately-
        // quoted regions (similar to shell), for example:
        //
        // <''>
        //
        // Can be quoted as:
        //
        // '<'"''"'>'
        //
        for (size_t i (0); i != n; ++i)
        {
          string& v (s[i].value);

          // Check if the quoting is required for this value.
          //
          if (!parser::need_cmdline_relex (v))
            continue;

          // If the value doesn't contain the single-quote character, then
          // single-quote it.
          //
          size_t p (v.find ('\''));

          if (p == string::npos)
          {
            v = '\'' + v + '\'';
            continue;
          }

          // Otherwise quote the regions.
          //
          // Note that we double-quote the single-quote character sequences
          // and single-quote all the other regions.
          //
          string r;
          char q (p == 0 ? '"' : '\''); // Current region quoting mode.

          r += q; // Open the first region.

          for (char c: v)
          {
            // If we are in the double-quoting mode, then switch to the
            // single-quoting mode if a non-single-quote character is
            // encountered.
            //
            if (q == '"')
            {
              if (c != '\'')
              {
                r += q;   // Close the double-quoted region.
                q = '\''; // Set the single-quoting mode.
                r += q;   // Open the single-quoted region.
              }
            }
            //
            // If we are in the single-quoting mode, then switch to the
            // double-quoting mode if the single-quote character is
            // encountered.
            //
            else
            {
              if (c == '\'')
              {
                r += q;  // Close the single-quoted region.
                q = '"'; // Set the double-quoting mode.
                r += q;  // Open the double-quoted region.
              }
            }

            r += c;
          }

          r += q; // Close the last region.

          v = move (r);
        }

        assign (root.cmd_var) = move (s);
      }

      // group
      //
      void group::
      set_timeout (const string& t, bool success, const location& l)
      {
        const char* gt (parent != nullptr
                        ? "test group timeout"
                        : "testscript timeout");

        const char* tt ("test timeout");
        const char* pf ("timeout: ");

        size_t p (t.find ('/'));
        if (p != string::npos)
        {
          // Note: either of the timeouts can be omitted but not both.
          //
          if (t.size () == 1)
            fail (l) << "invalid timeout '" << t << "'";

          if (p != 0)
            group_deadline =
              to_deadline (parse_deadline (string (t, 0, p), gt, pf, l),
                           success);

          if (p != t.size () - 1)
            test_timeout =
              to_timeout (parse_timeout (string (t, p + 1), tt, pf, l),
                          success);
        }
        else
          group_deadline = to_deadline (parse_deadline (t, gt, pf, l),
                                        success);
      }

      optional<deadline> group::
      effective_deadline ()
      {
        return parent != nullptr
               ? earlier (parent->effective_deadline (), group_deadline)
               : group_deadline;
      }

      // test
      //
      void test::
      set_timeout (const string& t, bool success, const location& l)
      {
        fragment_deadline =
          to_deadline (
            parse_deadline (t, "test fragment timeout", "timeout: ", l),
            success);
      }

      optional<deadline> test::
      effective_deadline ()
      {
        if (!test_deadline)
        {
          assert (parent != nullptr); // Test is always inside a group scope.

          test_deadline = parent->effective_deadline ();

          // Calculate the minimum timeout and factor it into the resulting
          // deadline.
          //
          optional<timeout> t (root.test_timeout); // config.test.timeout
          for (const scope* p (parent); p != nullptr; p = p->parent)
          {
            const group* g (dynamic_cast<const group*> (p));
            assert (g != nullptr);

            t = earlier (t, g->test_timeout);
          }

          if (t)
            test_deadline =
              earlier (*test_deadline,
                       deadline (system_clock::now () + t->value, t->success));
        }

        return earlier (*test_deadline, fragment_deadline);
      }
    }
  }
}
