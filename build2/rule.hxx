// file      : build2/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_RULE_HXX
#define BUILD2_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/target.hxx>
#include <build2/operation.hxx>

namespace build2
{
  // Once a rule is registered (for a scope), it is treated as immutable. If
  // you need to modify some state (e.g., counters or some such), then make
  // sure it is MT-safe.
  //
  // Note: match() is only called once but may not be followed by apply().
  //
  class rule
  {
  public:
    virtual bool
    match (action, target&, const string& hint) const = 0;

    virtual recipe
    apply (action, target&) const = 0;
  };

  // Fallback rule that only matches if the file exists.
  //
  class file_rule: public rule
  {
  public:
    file_rule () {}

    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    static const file_rule instance;
  };

  class alias_rule: public rule
  {
  public:
    alias_rule () {}

    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    static const alias_rule instance;
  };

  class fsdir_rule: public rule
  {
  public:
    fsdir_rule () {}

    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    static target_state
    perform_update (action, const target&);

    static target_state
    perform_clean (action, const target&);

    // Sometimes, as an optimization, we want to emulate execute_direct()
    // of fsdir{} without the overhead of switching to the execute phase.
    //
    static void
    perform_update_direct (action, const target&);

    static const fsdir_rule instance;
  };

  // Fallback rule that always matches and does nothing.
  //
  class fallback_rule: public build2::rule
  {
  public:
    fallback_rule () {}

    virtual bool
    match (action, target&, const string&) const override
    {
      return true;
    }

    virtual recipe
    apply (action, target&) const override {return noop_recipe;}

    static const fallback_rule instance;
  };
}

#endif // BUILD2_RULE_HXX
