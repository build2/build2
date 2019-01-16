// file      : build2/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_RULE_HXX
#define BUILD2_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/action.hxx>
#include <build2/target.hxx>

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

  // Fallback rule that only matches if the file exists. It will also match
  // an mtime_target provided it has a set timestamp.
  //
  class file_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    file_rule () {}
    static const file_rule instance;
  };

  class alias_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    alias_rule () {}
    static const alias_rule instance;
  };

  class fsdir_rule: public rule
  {
  public:
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

    fsdir_rule () {}
    static const fsdir_rule instance;
  };

  // Fallback rule that always matches and does nothing.
  //
  class noop_rule: public rule
  {
  public:
    virtual bool
    match (action, target&, const string&) const override;

    virtual recipe
    apply (action, target&) const override;

    noop_rule () {}
    static const noop_rule instance;
  };
}

#endif // BUILD2_RULE_HXX
