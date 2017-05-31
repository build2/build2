// file      : build2/bin/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/rule.hxx>

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/algorithm.hxx>
#include <build2/diagnostics.hxx>

#include <build2/bin/target.hxx>

using namespace std;

namespace build2
{
  namespace bin
  {
    // obj
    //
    match_result obj_rule::
    match (action a, target& t, const string&) const
    {
      fail << diag_doing (a, t) << " target group" <<
        info << "explicitly select obje{}, obja{}, or objs{} member";

      return false;
    }

    recipe obj_rule::
    apply (action, target&) const {return empty_recipe;}

    // bmi
    //
    match_result bmi_rule::
    match (action a, target& t, const string&) const
    {
      fail << diag_doing (a, t) << " target group" <<
        info << "explicitly select bmie{}, bmia{}, or bmis{} member";

      return false;
    }

    recipe bmi_rule::
    apply (action, target&) const {return empty_recipe;}

    // lib
    //
    // The whole logic is pretty much as if we had our two group members as
    // our prerequisites.
    //
    match_result lib_rule::
    match (action act, target& xt, const string&) const
    {
      lib& t (xt.as<lib> ());

      // Get the library type to build. If not set for a target, this should
      // be configured at the project scope by init().
      //
      const string& type (cast<string> (t["bin.lib"]));

      bool a (type == "static" || type == "both");
      bool s (type == "shared" || type == "both");

      if (!a && !s)
        fail << "unknown library type: " << type <<
          info << "'static', 'shared', or 'both' expected";

      t.a = a ? &search<liba> (t, t.dir, t.out, t.name) : nullptr;
      t.s = s ? &search<libs> (t, t.dir, t.out, t.name) : nullptr;

      match_result mr (true);

      // If there is an outer operation, indicate that we match
      // unconditionally so that we don't override ourselves.
      //
      if (act.outer_operation () != 0)
        mr.recipe_action = action (act.meta_operation (), act.operation ());

      return mr;
    }

    recipe lib_rule::
    apply (action act, target& xt) const
    {
      lib& t (xt.as<lib> ());

      const target* m[] = {t.a, t.s};
      match_members (act, t, m);

      return &perform;
    }

    target_state lib_rule::
    perform (action act, const target& xt)
    {
      const lib& t (xt.as<lib> ());

      const target* m[] = {t.a, t.s};
      return execute_members (act, t, m);
    }
  }
}
