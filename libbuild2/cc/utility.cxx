// file      : libbuild2/cc/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/utility.hxx>

#include <libbuild2/file.hxx>

using namespace std;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    const dir_path module_dir ("cc");
    const dir_path module_build_dir (dir_path (module_dir) /= "build");
    const dir_path module_build_modules_dir (
      dir_path (module_build_dir) /= "modules");
  }
}
