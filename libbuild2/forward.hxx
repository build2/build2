// file      : libbuild2/forward.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_FORWARD_HXX
#define LIBBUILD2_FORWARD_HXX

#include <libbuild2/types.hxx>

namespace build2
{
  // Forward declarations for the build state.

  // <libbuild2/action.hxx>
  //
  struct action;

  // <libbuild2/spec.hxx>
  //
  struct opspec;

  // <libbuild2/variable.hxx>
  //
  class value;
  using values = small_vector<value, 1>;
  struct lookup;

  struct variable;
  class variable_pool;
  class variable_map;
  struct variable_override;
  using variable_overrides = vector<variable_override>;
  class variable_override_cache;

  // <libbuild2/function.hxx>
  //
  class function_map;

  // <libbuild2/scope.hxx>
  //
  class scope;
  class scope_map;

  // <libbuild2/target-type.hxx>
  //
  class target_type_map;

  // <libbuild2/target-key.hxx>
  //
  class target_key;

  // <libbuild2/target.hxx>
  //
  class target;
  class target_set;
  class include_type;
  struct prerequisite_member;

  // <libbuild2/prerequisite.hxx>
  //
  class prerequisite;
  class prerequisite_key;

  // <libbuild2/rule.hxx>
  //
  class rule;

  // <libbuild2/context.hxx>
  //
  class context;
}

#endif // LIBBUILD2_FORWARD_HXX
