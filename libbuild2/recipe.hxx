// file      : libbuild2/recipe.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_RECIPE_HXX
#define LIBBUILD2_RECIPE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>
#include <libbuild2/target-state.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // The returned target state is normally changed or unchanged. If there is
  // an error, then the recipe should throw failed rather than returning (this
  // is the only exception that a recipe can throw).
  //
  // The return value of the recipe is used to update the target state. If it
  // is target_state::group then the target's state is the group's state.
  //
  // The recipe may also return postponed in which case the target state is
  // assumed to be unchanged (normally this means a prerequisite was postponed
  // and while the prerequisite will be re-examined via another dependency,
  // this target is done).
  //
  // Note that max size for the "small size optimization" in std::function
  // (which is what move_only_function_ex is based on) ranges (in pointer
  // sizes) from 0 (GCC libstdc++ prior to 5) to 2 (GCC 5 and later) to 3
  // (Clang libc++) to 6 (VC 14.3). With the size ranging (in bytes for 64-bit
  // target) from 32 (GCC) to 64 (VC).
  //
  using recipe_function = target_state (action, const target&);
  using recipe = move_only_function_ex<recipe_function>;

  // Commonly-used recipes.
  //
  // The default recipe executes the action on all the prerequisites in a
  // loop, skipping ignored. Specifically, for actions with the "first"
  // execution mode, it calls execute_prerequisites() while for those with
  // "last" -- reverse_execute_prerequisites() (see <libbuild2/operation.hxx>,
  // <libbuild2/algorithm.hxx> for details). The group recipe calls the
  // group's recipe.
  //
  LIBBUILD2_SYMEXPORT extern recipe_function* const empty_recipe;
  LIBBUILD2_SYMEXPORT extern recipe_function* const noop_recipe;
  LIBBUILD2_SYMEXPORT extern recipe_function* const default_recipe;
  LIBBUILD2_SYMEXPORT extern recipe_function* const group_recipe;
  LIBBUILD2_SYMEXPORT extern recipe_function* const inner_recipe;
}

#endif // LIBBUILD2_RECIPE_HXX
