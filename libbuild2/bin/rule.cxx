// file      : libbuild2/bin/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bin/rule.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>
#include <libbuild2/bin/utility.hxx>

using namespace std;

namespace build2
{
  namespace bin
  {
    // obj_rule
    //
    bool obj_rule::
    match (action a, target& t, const string&) const
    {
      const char* n (t.dynamic_type ().name); // Ignore derived type.

      fail << diag_doing (a, t) << " target group" <<
        info << "explicitly select " << n << "e{}, " << n << "a{}, or "
           << n << "s{} member" << endf;
    }

    recipe obj_rule::
    apply (action, target&) const {return empty_recipe;}

    // libul_rule
    //
    bool libul_rule::
    match (action a, target& t, const string&) const
    {
      fail << diag_doing (a, t) << " target group" <<
        info << "explicitly select libua{} or libus{} member" << endf;
    }

    recipe libul_rule::
    apply (action, target&) const {return empty_recipe;}

    // lib_rule
    //
    // The whole logic is pretty much as if we had our two group members as
    // our prerequisites.
    //
    bool lib_rule::
    match (action a, target& xt, const string&) const
    {
      lib& t (xt.as<lib> ());

      lmembers bm (a.meta_operation () != dist_id
                   ? link_members (t.root_scope ())
                   : lmembers {true, true});

      t.a = bm.a ? &search<liba> (t, t.dir, t.out, t.name) : nullptr;
      t.s = bm.s ? &search<libs> (t, t.dir, t.out, t.name) : nullptr;

      return true;
    }

    recipe lib_rule::
    apply (action a, target& xt) const
    {
      lib& t (xt.as<lib> ());

      //@@ outer: also prerequisites (if outer) or not?

      const target* m[] = {t.a, t.s};
      match_members (a, t, m);

      return &perform;
    }

    target_state lib_rule::
    perform (action a, const target& xt)
    {
      const lib& t (xt.as<lib> ());

      const target* m[] = {t.a, t.s};
      return execute_members (a, t, m);
    }
  }
}
