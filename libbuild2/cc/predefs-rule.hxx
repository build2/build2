// file      : libbuild2/cc/predefs-rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_PREDEFS_RULE_HXX
#define LIBBUILD2_CC_PREDEFS_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/dyndep.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/common.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  class json_value;

  namespace cc
  {
    class compile_rule;

    class LIBBUILD2_CC_SYMEXPORT predefs_rule: public rule,
                                               virtual common,
                                               dyndep_rule
    {
    public:
      const string rule_name;

      predefs_rule (data&&, const compile_rule&);

      struct match_data;

      virtual bool
      match (action, target&, const string&, match_extra&) const override;

      virtual recipe
      apply (action, target&, match_extra&) const override;

      target_state
      perform_update (action, const target&, match_data&) const;

    private:
      void
      read_gcc (diag_buffer&, ifdstream&,
                const function<void (string, const json_value&)>&,
                const function<void (path)>&,
                match_data&) const;

      bool
      read_msvc (diag_buffer&, ifdstream&,
                 const function<void (string, const json_value&)>&,
                 const function<void (path)>&,
                 match_data&,
                 ofdstream&,
                 const path&) const;

      void
      write_macro_buildfile (ofdstream&,
                             const string& n, const json_value&) const;

      void
      write_macro_json (json_buffer_serializer&,
                        const string&, const json_value&) const;

    private:
      const string rule_id;
      const compile_rule& c_rule;
    };
  }
}

#endif // LIBBUILD2_CC_PREDEFS_RULE_HXX
