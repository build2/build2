// file      : build/bin/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/bin/rule>

#include <build/scope>
#include <build/target>
#include <build/algorithm>
#include <build/diagnostics>

#include <build/bin/target>

using namespace std;

namespace build
{
  namespace bin
  {
    // obj
    //
    void* obj_rule::
    match (action a, target& t, const std::string&) const
    {
      fail << diag_doing (a, t) << " target group" <<
        info << "explicitly select either obja{} or objso{} member";

      return nullptr;
    }

    recipe obj_rule::
    apply (action, target&, void*) const {return empty_recipe;}

    // lib
    //
    // The whole logic is pretty much as if we had our two group
    // members as prerequisites.
    //
    void* lib_rule::
    match (action, target& t, const std::string&) const
    {
      return &t;
    }

    recipe lib_rule::
    apply (action a, target& xt, void*) const
    {
      lib& t (static_cast<lib&> (xt));

      // Get the library type to build. If not set for a target, this
      // should be configured at the project scope by init_lib().
      //
      const string& type (t["bin.lib"].as<const string&> ());

      bool ar (type == "static" || type == "both");
      bool so (type == "shared" || type == "both");

      if (!ar && !so)
        fail << "unknown library type: " << type <<
          info << "'static', 'shared', or 'both' expected";

      if (ar)
      {
        if (t.a == nullptr)
          t.a = &static_cast<liba&> (search (prerequisite_key {
                {&liba::static_type, &t.dir, &t.name, &t.ext}, nullptr}));

        build::match (a, *t.a);
      }

      if (so)
      {
        if (t.so == nullptr)
          t.so = &static_cast<libso&> (search (prerequisite_key {
                {&libso::static_type, &t.dir, &t.name, &t.ext}, nullptr}));

        build::match (a, *t.so);
      }

      // Search and match prerequisite libraries and add them to the
      // prerequisite targets. While we never execute this list
      // ourselves (see perform() below), this is necessary to make
      // the exported options machinery work for the library chains.
      // See cxx.export.*-related code in cxx/rule.cxx for details.
      //
      for (prerequisite& p: group_prerequisites (t))
      {
        if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libso> ())
        {
          target& pt (search (p));
          build::match (a, pt);
          t.prerequisite_targets.push_back (&pt);
        }
      }

      return &perform;
    }

    target_state lib_rule::
    perform (action a, target& xt)
    {
      lib& t (static_cast<lib&> (xt));

      //@@ Not cool we have to do this again. Looks like we need
      //   some kind of a cache vs resolved pointer, like in
      //   prerequisite vs prerequisite_target.
      //
      //
      const string& type (t["bin.lib"].as<const string&> ());
      bool ar (type == "static" || type == "both");
      bool so (type == "shared" || type == "both");

      target* m1 (ar ? t.a : nullptr);
      target* m2 (so ? t.so : nullptr);

      if (current_mode == execution_mode::last)
        swap (m1, m2);

      target_state ts (target_state::unchanged);

      if (m1 != nullptr && execute (a, *m1) == target_state::changed)
        ts = target_state::changed;

      if (m2 != nullptr && execute (a, *m2) == target_state::changed)
        ts = target_state::changed;

      return ts;
    }
  }
}
