// file      : libbuild2/test/script/script.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/script/script.hxx>

#include <sstream>

#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>

#include <libbuild2/script/parser.hxx> // parser::apply_value_attributes()

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      // scope_base
      //
      scope_base::
      scope_base (script& s)
          : root (s),
            vars (s.test_target.ctx, false /* global */)
      {
        vars.assign (root.wd_var) = dir_path ();
      }

      const dir_path& scope_base::
      wd_path () const
      {
        return cast<dir_path> (vars [root.wd_var]);
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
      static const string wd_name ("test working directory");
      static const string sd_name ("working directory");

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
                         wd_path (), wd_name,
                         p != nullptr ? root.work_dir : wd_path (), sd_name,
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
          const_cast<dir_path&> (work_dir) = dir_path (p->work_dir) /= id;
      }

      void scope::
      set_variable (string&& nm, names&& val, const string& attrs)
      {
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
          build2::script::parser p (context);
          p.apply_value_attributes (&var,
                                    lhs,
                                    value (move (val)),
                                    attrs,
                                    token_type::assign,
                                    path_name ("<attributes>"));
        }
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
            redirects_var (var_pool.insert<strings> ("test.redirects")),
            cleanups_var  (var_pool.insert<strings> ("test.cleanups")),

            wd_var (var_pool.insert<dir_path> ("~")),
            id_var (var_pool.insert<path> ("@")),
            cmd_var (var_pool.insert<strings> ("*")),
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
      script (const target& tt,
              const testscript& st,
              const dir_path& rwd)
          : script_base (tt, st),
            group (st.name == "testscript" ? string () : st.name, *this)
      {
        // Set the script working dir ($~) to $out_base/test/<id> (id_path
        // for root is just the id which is empty if st is 'testscript').
        //
        const_cast<dir_path&> (work_dir) = dir_path (rwd) /= id_path.string ();

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
              // Must be a target name.
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

        // Set the special $*, $N variables.
        //
        reset_special ();
      }

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
        const variable* pvar (context.var_pool.find (n));

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
        // First assemble the $* value.
        //
        strings s;

        auto append = [&s] (const strings& v)
        {
          s.insert (s.end (), v.begin (), v.end ());
        };

        if (auto l = lookup (root.test_var))
          s.push_back (cast<path> (l).representation ());

        if (auto l = lookup (root.options_var))
          append (cast<strings> (l));

        if (auto l = lookup (root.arguments_var))
          append (cast<strings> (l));

        // Keep redirects/cleanups out of $N.
        //
        size_t n (s.size ());

        if (auto l = lookup (root.redirects_var))
          append (cast<strings> (l));

        if (auto l = lookup (root.cleanups_var))
          append (cast<strings> (l));

        // Set the $N values if present.
        //
        for (size_t i (0); i <= 9; ++i)
        {
          value& v (assign (*root.cmdN_var[i]));

          if (i < n)
          {
            if (i == 0)
              v = path (s[i]);
            else
              v = s[i];
          }
          else
            v = nullptr; // Clear any old values.
        }

        // Set $*.
        //
        assign (root.cmd_var) = move (s);
      }
    }
  }
}
