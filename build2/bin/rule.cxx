// file      : build2/bin/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/rule>

#include <build2/scope>
#include <build2/target>
#include <build2/algorithm>
#include <build2/diagnostics>

#include <build2/bin/target>

using namespace std;

namespace build2
{
  namespace bin
  {
    // obj
    //
    match_result obj_rule::
    match (slock&, action a, target& t, const string&) const
    {
      fail << diag_doing (a, t) << " target group" <<
        info << "explicitly select obje{}, obja{}, or objs{} member";

      return false;
    }

    recipe obj_rule::
    apply (slock&, action, target&) const {return empty_recipe;}

    // lib
    //
    // The whole logic is pretty much as if we had our two group members as
    // our prerequisites.
    //

    struct match_data
    {
      const string& type;
    };

    static_assert (sizeof (match_data) <= target::data_size,
                   "insufficient space");

    match_result lib_rule::
    match (slock&, action act, target& xt, const string&) const
    {
      lib& t (xt.as<lib> ());

      // @@ We have to re-query it on each match_only()!

      // Get the library type to build. If not set for a target, this
      // should be configured at the project scope by init().
      //
      const string& type (cast<string> (t["bin.lib"]));

      bool a (type == "static" || type == "both");
      bool s (type == "shared" || type == "both");

      if (!a && !s)
        fail << "unknown library type: " << type <<
          info << "'static', 'shared', or 'both' expected";

      t.data (match_data {type}); // Save in the target's auxilary storage.

      match_result mr (true);

      // If there is an outer operation, indicate that we match
      // unconditionally so that we don't override ourselves.
      //
      if (act.outer_operation () != 0)
        mr.recipe_action = action (act.meta_operation (), act.operation ());

      return mr;
    }

    recipe lib_rule::
    apply (slock& ml, action act, target& xt) const
    {
      lib& t (xt.as<lib> ());

      const match_data& md (t.data<match_data> ());
      const string& type (md.type);

      bool a (type == "static" || type == "both");
      bool s (type == "shared" || type == "both");

      if (a)
      {
        if (t.a == nullptr)
          t.a = &search<liba> (t.dir, t.out, t.name, nullptr, nullptr);

        build2::match (ml, act, *t.a);
      }

      if (s)
      {
        if (t.s == nullptr)
          t.s = &search<libs> (t.dir, t.out, t.name, nullptr, nullptr);

        build2::match (ml, act, *t.s);
      }

      return &perform;
    }

    target_state lib_rule::
    perform (action act, const target& xt)
    {
      const lib& t (xt.as<lib> ());

      const match_data& md (t.data<match_data> ());
      const string& type (md.type);

      bool a (type == "static" || type == "both");
      bool s (type == "shared" || type == "both");

      const target* m[] = {a ? t.a : nullptr, s ? t.s : nullptr};
      return execute_members (act, t, m);
    }
  }
}
