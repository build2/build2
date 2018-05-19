// file      : build2/cli/target.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CLI_TARGET_HXX
#define BUILD2_CLI_TARGET_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/target.hxx>

#include <build2/cxx/target.hxx>

namespace build2
{
  namespace cli
  {
    class cli: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    // Standard layout type compatible with group_view's const target*[3].
    //
    struct cli_cxx_members
    {
      const cxx::hxx* h = nullptr;
      const cxx::cxx* c = nullptr;
      const cxx::ixx* i = nullptr;
    };

    class cli_cxx: public mtime_target, public cli_cxx_members
    {
    public:
      using mtime_target::mtime_target;

      virtual group_view
      group_members (action) const override;

    public:
      static const target_type static_type;

      virtual const target_type&
      dynamic_type () const override {return static_type;}
    };
  }
}

#endif // BUILD2_CLI_TARGET_HXX
