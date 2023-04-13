// file      : libbuild2/cli/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CLI_TARGET_HXX
#define LIBBUILD2_CLI_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

#include <libbuild2/cxx/target.hxx>

#include <libbuild2/cli/export.hxx>

namespace build2
{
  namespace cli
  {
    class LIBBUILD2_CLI_SYMEXPORT cli: public file
    {
    public:
      cli (context& c, dir_path d, dir_path o, string n)
          : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Standard layout type compatible with group_view's const target*[3].
    //
    struct cli_cxx_members
    {
      const cxx::hxx* h = nullptr;
      const cxx::cxx* c = nullptr;
      const cxx::ixx* i = nullptr;
    };

    class LIBBUILD2_CLI_SYMEXPORT cli_cxx: public mtime_target,
                                           public cli_cxx_members
    {
    public:
      cli_cxx (context& c, dir_path d, dir_path o, string n)
          : mtime_target (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

      virtual group_view
      group_members (action) const override;

    public:
      static const target_type static_type;
    };
  }
}

#endif // LIBBUILD2_CLI_TARGET_HXX
