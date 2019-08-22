// file      : libbuild2/search.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SEARCH_HXX
#define LIBBUILD2_SEARCH_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  class target;
  class context;
  class prerequisite_key;

  // Search for an existing target in this prerequisite's scope. Scope can be
  // NULL if directories are absolute.
  //
  LIBBUILD2_SYMEXPORT const target*
  search_existing_target (context&, const prerequisite_key&);

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
  LIBBUILD2_SYMEXPORT const target&
  create_new_target (context&, const prerequisite_key&);
}

#endif // LIBBUILD2_SEARCH_HXX
