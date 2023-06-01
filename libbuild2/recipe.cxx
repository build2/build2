// file      : libbuild2/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/target.hxx>

#include <libbuild2/algorithm.hxx>

namespace build2
{
  recipe_function* const empty_recipe   = nullptr;
  recipe_function* const noop_recipe    = &noop_action;
  recipe_function* const default_recipe = &default_action;
  recipe_function* const group_recipe   = &group_action;
  recipe_function* const inner_recipe   = &execute_inner;
}
