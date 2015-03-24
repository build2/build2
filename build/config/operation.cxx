// file      : build/config/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/config/operation>

#include <build/diagnostics>

using namespace std;

namespace build
{
  namespace config
  {
    meta_operation_info configure {"configure"};

    // disfigure
    //
    static operation_id
    disfigure_operation_pre (operation_id o)
    {
      return o; // Don't translate default to update.
    }

    static void
    disfigure_load (const path& bf,
                    scope&,
                    const path&,
                    const path&,
                    const location&)
    {
      tracer trace ("disfigure_load");
      level4 ([&]{trace << "skipping " << bf;});
    }

    static void
    disfigure_match (action a,
                     const target_key& tk,
                     const location& l,
                     action_targets& ts)
    {
      tracer trace ("disfigure_match");
      //level4 ([&]{trace << "matching " << t;});
      //ts.push_back (&t);
    }

    static void
    disfigure_execute (action a, const action_targets& ts)
    {
      tracer trace ("execute");


      for (void* v: ts)
      {
        //level4 ([&]{trace << "disfiguring target " << t;});
      }
    }

    meta_operation_info disfigure {
      "disfigure",
      nullptr, // meta-operation pre
      &disfigure_operation_pre,
      &disfigure_load,
      &disfigure_match,
      &disfigure_execute,
      nullptr, // operation post
      nullptr  // meta-operation post
    };
  }
}
