// file      : libbuild2/in/rule.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_IN_RULE_HXX
#define LIBBUILD2_IN_RULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx> // depdb
#include <libbuild2/utility.hxx>

#include <libbuild2/rule.hxx>

#include <libbuild2/in/export.hxx>

namespace build2
{
  namespace in
  {
    // Preprocess an .in file.
    //
    // Note that a derived rule can use the target auxiliary data storage to
    // cache data (e.g., in match() or apply()) to be used in substitute() and
    // lookup() calls.
    //
    // A derived rule is also required to derive the target file name in
    // match() instead of apply() to make it available early for the in{}
    // prerequisite search (see install::file_rule::apply_impl() for
    // background).
    //
    // Note also that currently this rule ignores the dry-run mode (see
    // perform_update() for the rationale).
    //
    class LIBBUILD2_IN_SYMEXPORT rule: public simple_rule
    {
    public:
      // The rule id is used to form the rule name/version entry in depdb. The
      // program argument is the pseudo-program name to use in the command
      // line diagnostics.
      //
      rule (string rule_id,
            string program,
            char symbol = '$',
            bool strict = true,
            optional<string> null = nullopt)
          : rule_id_ (move (rule_id)),
            program_ (move (program)),
            symbol_ (symbol),
            strict_ (strict),
            null_ (move (null)) {}

      virtual bool
      match (action, target&) const override;

      virtual recipe
      apply (action, target&) const override;

      virtual target_state
      perform_update (action, const target&) const;

      // Customization hooks and helpers.
      //
      using substitution_map = map<string, optional<string>>;

      // Perform prerequisite search.
      //
      virtual prerequisite_target
      search (action, const target&,
              const prerequisite_member&,
              include_type) const;

      // Additional depdb entries.
      //
      virtual void
      perform_update_depdb (action, const target&, depdb&) const;

      // Pre/post update.
      //
      virtual void
      perform_update_pre (action, const target&,
                          ofdstream&, const char* newline) const;

      virtual void
      perform_update_post (action, const target&,
                           ofdstream&, const char* newline) const;

      // Perform variable lookup.
      //
      // Flags can be used by a custom implementation to alter the lookup
      // semantics, for example, for special substitutions. Note, however,
      // that one must make sure this semantics cannot change without changes
      // to the .in file (see the depdb logic for details).
      //
      virtual string
      lookup (const location&,
              action, const target&,
              const string& name,
              optional<uint64_t> flags,
              const substitution_map*,
              const optional<string>& null) const;

      // Perform variable substitution. Return nullopt if it should be
      // ignored.
      //
      virtual optional<string>
      substitute (const location&,
                  action, const target&,
                  const string& name,
                  optional<uint64_t> flags,
                  bool strict,
                  const substitution_map*,
                  const optional<string>& null) const;

      // Call the above version and do any necessary depdb saving.
      //
      optional<string>
      substitute (const location&,
                  action, const target&,
                  depdb& dd, size_t& dd_skip,
                  const string& name,
                  optional<uint64_t> flags,
                  bool strict,
                  const substitution_map*,
                  const optional<string>& null) const;

      // Process a line of input from the specified position performing any
      // necessary substitutions.
      //
      virtual void
      process (const location&,
               action, const target&,
               depdb& dd, size_t& dd_skip,
               string& line, size_t pos,
               const char* newline,
               char sym,
               bool strict,
               const substitution_map*,
               const optional<string>& null) const;

      // Replace newlines in a multi-line value with the given newline
      // sequence.
      //
      static void
      replace_newlines (string& v, const char* newline)
      {
        for (size_t p (0), n; (p = v.find ('\n', p)) != string::npos; p += n)
        {
          n = 1;

          // Deal with CRLF in the value.
          //
          if (p != 0 && v[p - 1] == '\r')
          {
            --p;
            ++n;
          }

          v.replace (p, n, newline);
        }
      }

    protected:
      const string rule_id_;
      const string program_;
      char symbol_;
      bool strict_;
      optional<string> null_;
    };
  }
}

#endif // LIBBUILD2_IN_RULE_HXX
