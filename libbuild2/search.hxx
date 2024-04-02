// file      : libbuild2/search.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SEARCH_HXX
#define LIBBUILD2_SEARCH_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Search for an existing target in this prerequisite's scope. Scope can be
  // NULL if directories are absolute.
  //
  // If dir is relative and out is not specified, then first search in the out
  // tree and, if not found, then in the src tree, unless out_only is true.
  // If dir is absolute, then out is expected to be specified as well, if
  // necessary.
  //
  LIBBUILD2_SYMEXPORT const target*
  search_existing_target (context&, const prerequisite_key&, bool out_only);

  // Search for an existing file. If the prerequisite directory is relative,
  // then look in the scope's src directory. Otherwise, if the absolute
  // directory is inside the project's root scope, look there. In case of
  // the absolute directory, if the scope is NULL, assume the file is not
  // in src.
  //
  // Originally the plan was to have a target-type specific variable that
  // contains the search paths. But there wasn't any need for this yet.
  //
  LIBBUILD2_SYMEXPORT const target*
  search_existing_file (context&, const prerequisite_key&);

  // Create a new target in this prerequisite's scope.
  //
  // Fail if the target is in src directory.
  //
  LIBBUILD2_SYMEXPORT const target&
  create_new_target (context&, const prerequisite_key&);

  // As above but return the lock if the target was newly created.
  //
  LIBBUILD2_SYMEXPORT pair<target&, ulock>
  create_new_target_locked (context&, const prerequisite_key&);
}

#endif // LIBBUILD2_SEARCH_HXX
