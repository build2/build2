// file      : build2/cc/install.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_INSTALL_HXX
#define BUILD2_CC_INSTALL_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/install/rule.hxx>

#include <build2/cc/types.hxx>
#include <build2/cc/common.hxx>

namespace build2
{
  namespace cc
  {
    class link;

    // Installation rule for exe{}, lib*{}, etc.
    //
    class file_install: public install::file_rule, virtual common
    {
    public:
      file_install (data&&, const link&);

      virtual const target*
      filter (action, const target&, prerequisite_member) const override;

      virtual match_result
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      virtual void
      install_extra (const file&, const install_dir&) const override;

      virtual bool
      uninstall_extra (const file&, const install_dir&) const override;

    private:
      const link& link_;
    };

    // Installation rule for libux{}.
    //
    class alias_install: public install::alias_rule, virtual common
    {
    public:
      alias_install (data&&, const link&);

      virtual const target*
      filter (action, const target&, prerequisite_member) const override;

      virtual match_result
      match (action, target&, const string&) const override;

    private:
      const link& link_;
    };
  }
}

#endif // BUILD2_CC_INSTALL_HXX
