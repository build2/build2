// file      : build/test/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/test/module>

#include <build/scope>
#include <build/target>
#include <build/rule>
#include <build/diagnostics>

#include <build/test/operation>
#include <build/test/rule>

using namespace std;
using namespace butl;

namespace build
{
  namespace test
  {
    class module: public build::module
    {
    public:
      module (operation_id test_id): rule (test_id) {}

      test::rule rule;
    };

    extern "C" void
    test_init (scope& root,
               scope& base,
               const location& l,
               unique_ptr<build::module>& r,
               bool first)
    {
      tracer trace ("test::init");

      if (&root != &base)
        fail (l) << "test module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple test module initializations";
        return;
      }

      const dir_path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Register the test operation.
      //
      operation_id test_id (root.operations.insert (test));

      unique_ptr<module> m (new module (test_id));

      {
        auto& rs (base.rules);

        // Register the standard alias rule for the test operation.
        //
        rs.insert<alias> (test_id, "alias", alias_rule::instance);

        // Register our test running rule.
        //
        rs.insert<target> (test_id, "test", m->rule);
      }

      r = move (m);
    }
  }
}
