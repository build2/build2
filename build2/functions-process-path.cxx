// file      : build2/functions-process-path.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function.hxx>
#include <build2/variable.hxx>

using namespace std;

namespace build2
{
  void
  process_path_functions ()
  {
    function_family f ("process_path");

    // As discussed in value_traits<process_path>, we always have recall.
    //
    f["recall"] = &process_path::recall;
    f["effect"] = [](process_path p)
    {
      return move (p.effect.empty () ? p.recall : p.effect);
    };
  }
}
