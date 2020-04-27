// file      : build2/cli/module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CLI_MODULE_HXX
#define BUILD2_CLI_MODULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <build2/cli/rule.hxx>

namespace build2
{
  namespace cli
  {
    class module: public build2::module,
                  public virtual data,
                  public compile_rule
    {
    public:
      explicit
      module (data&& d)
          : data (move (d)), compile_rule (move (d)) {}
    };
  }
}

#endif // BUILD2_CLI_MODULE_HXX
