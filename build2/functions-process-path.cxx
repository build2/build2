// file      : build2/functions-process-path.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function>
#include <build2/variable>

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

    //@@ TMP kludge
    //
    f["effect"] = [](names n)
      {
        auto p (value_traits<process_path>::convert (
                  move (n[0]), n.size () > 1 ? &n[1] : nullptr));

        return move (p.effect.empty () ? p.recall : p.effect);
      };
  }
}
