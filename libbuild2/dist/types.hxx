// file      : libbuild2/dist/types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DIST_TYPES_HXX
#define LIBBUILD2_DIST_TYPES_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>

#include <libbuild2/prerequisite-key.hxx>

namespace build2
{
  namespace dist
  {
    // List of prerequisites that could not be searched to a target and were
    // postponed for later re-search. This can happen, for example, because a
    // prerequisite would resolve to a member of a group that hasn't been
    // matched yet (for example, libs{} of lib{}). See rule::apply() for
    // details.
    //
    // Note that we are using list instead of vector because new elements can
    // be added at the end while we are iterating over the list.
    //
    struct postponed_prerequisite
    {
      build2::action                          action;
      reference_wrapper<const build2::target> target;
      reference_wrapper<const prerequisite>   prereq;
      string                                  rule;
    };

    struct postponed_prerequisites
    {
      build2::mutex                        mutex;
      build2::list<postponed_prerequisite> list;
    };
  }
}

#endif // LIBBUILD2_DIST_TYPES_HXX
