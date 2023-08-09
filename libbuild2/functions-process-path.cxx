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
    function_family f (m, "process_path");

    // $recall(<process-path>)
    //
    // Return the recall path of an executable, that is, a path that is not
    // necessarily absolute but which nevertheless can be used to re-run the
    // executable in the current environment. This path, for example, could be
    // used in diagnostics when printing the failing command line.
    //

    // As discussed in value_traits<process_path>, we always have recall.
    //
    f["recall"] += &process_path::recall;


    // $effect(<process-path>)
    //
    // Return the effective path of an executable, that is, the absolute path
    // to the executable that will also include any omitted extensions, etc.
    //
    f["effect"] += [] (process_path p)
    {
      return move (p.effect.empty () ? p.recall : p.effect);
    };

    // $name(<process-path-ex>)
    //
    // Return the stable process name for diagnostics.
    //
    f["name"] += &process_path_ex::name;

    // $checksum(<process-path-ex>)
    //
    // Return the executable checksum for change tracking.
    //
    f["checksum"] += &process_path_ex::checksum;

    // $env_checksum(<process-path-ex>)
    //
    // Return the environment checksum for change tracking.
    //
    f["env_checksum"] += &process_path_ex::env_checksum;
  }
}
