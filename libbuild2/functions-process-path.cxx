// file      : libbuild2/functions-process-path.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  process_path_functions (function_map& m)
  {
    {
      function_family f (m, "process_path");

      // As discussed in value_traits<process_path>, we always have recall.
      //
      f["recall"] += &process_path::recall;
      f["effect"] += [](process_path p)
      {
        return move (p.effect.empty () ? p.recall : p.effect);
      };
    }

    {
      function_family f (m, "process_path_ex");

      f["name"] += &process_path_ex::name;
      f["checksum"] += &process_path_ex::checksum;
      f["env_checksum"] += &process_path_ex::env_checksum;
    }
  }
}
