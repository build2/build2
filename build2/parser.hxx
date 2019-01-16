// file      : build2/parser.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_PARSER_HXX
#define BUILD2_PARSER_HXX

#include <stack>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/spec.hxx>
#include <build2/lexer.hxx>
#include <build2/token.hxx>
#include <build2/variable.hxx>
#include <build2/diagnostics.hxx>

namespace build2
{
  class scope;
  class target;
  class prerequisite;

  class parser
  {
  public:
    // If boot is true, then we are parsing bootstrap.build and modules
    // should only be bootstrapped.
    //
    explicit
    parser (bool boot = false): fail ("error", &path_), boot_ (boot) {}

    // Issue diagnostics and throw failed in case of an error.
    //
    void
    parse_buildfile (istream&, const path& name, scope& root, scope& base);

    buildspec
    parse_buildspec (istream&, const path& name);

    token
    parse_variable (lexer&, scope&, const variable&, token_type kind);

    pair<value, token>
    parse_variable_value (lexer&, scope&, const dir_path*, const variable&);

    names
    parse_export_stub (istream& is, const path& p, scope& r, scope& b)
    {
      parse_buildfile (is, p, r, b);
      return move (export_value_);
    }

    // Recursive descent parser.
    //
  protected:

    // Pattern expansion mode.
    //
    enum class pattern_mode
    {
      ignore, // Treat as ordinary names.
      detect, // Ignore pair/dir/type if the first name is a pattern.
      expand  // Expand to ordinary names.
    };

    // If one is true then parse a single (logical) line (logical means it
    // can actually be several lines, e.g., an if-block). Return false if
    // nothing has been parsed (i.e., we are still on the same token).
    //
    // Note that after this function returns, the token is the first token of
    // the next line (or eos).
    //
    bool
    parse_clause (token&, token_type&, bool one = false);

    void
    parse_variable_block (token&, token_type&, const target_type*, string);

    void
    parse_dependency (token&, token_type&,
                      names&&, const location&,
                      names&&, const location&);

    void
    parse_assert (token&, token_type&);

    void
    parse_print (token&, token_type&);

    void
    parse_diag (token&, token_type&);

    void
    parse_dump (token&, token_type&);

    void
    parse_source (token&, token_type&);

    void
    parse_include (token&, token_type&);

    void
    parse_run (token&, token_type&);

    void
    parse_import (token&, token_type&);

    void
    parse_export (token&, token_type&);

    void
    parse_using (token&, token_type&);

    void
    parse_define (token&, token_type&);

    void
    parse_if_else (token&, token_type&);

    void
    parse_for (token&, token_type&);

    void
    parse_variable (token&, token_type&, const variable&, token_type);

    void
    parse_type_pattern_variable (token&, token_type&,
                                 const target_type&, string,
                                 const variable&, token_type, const location&);

    const variable&
    parse_variable_name (names&&, const location&);

    // Note: calls attributes_push() that the caller must pop.
    //
    value
    parse_variable_value (token&, token_type&);

    void
    apply_variable_attributes (const variable&);

    void
    apply_value_attributes (const variable*, // Optional.
                            value& lhs,
                            value&& rhs,
                            token_type assign_kind);

    // Return the value pack (values can be NULL/typed). Note that for an
    // empty eval context ('()' potentially with whitespaces in between) the
    // result is an empty pack, not a pack of one empty.
    //
    values
    parse_eval (token&, token_type&, pattern_mode);

    values
    parse_eval_comma (token&, token_type&, pattern_mode, bool = false);

    value
    parse_eval_ternary (token&, token_type&, pattern_mode, bool = false);

    value
    parse_eval_or (token&, token_type&, pattern_mode, bool = false);

    value
    parse_eval_and (token&, token_type&, pattern_mode, bool = false);

    value
    parse_eval_comp (token&, token_type&, pattern_mode, bool = false);

    value
    parse_eval_value (token&, token_type&, pattern_mode, bool = false);

    // Attributes stack. We can have nested attributes, for example:
    //
    // x = [bool] ([uint64] $x == [uint64] $y)
    //
    // In this example we only apply the value attributes after evaluating
    // the context, which has its own attributes.
    //
    struct attributes
    {
      bool has;                         // Has attributes flag.
      location loc;                     // Start of attributes location.
      vector<pair<string, string>> ats; // Attributes.

      explicit operator bool () const {return has;}
    };

    // Push a new entry into the attributes_ stack. If the next token is '['
    // parse the attribute sequence until ']' setting the 'has' flag and
    // storing the result on the stack. Then get the next token and, if
    // standalone is false, verify it is not newline/eos (i.e., there is
    // something after it). Return the indication of whether there are
    // any attributes and their location.
    //
    // Note that during pre-parsing nothing is pushed into the stack and
    // the returned attributes object indicates there are no attributes.
    //
    pair<bool, location>
    attributes_push (token&, token_type&, bool standalone = false);

    attributes
    attributes_pop ()
    {
      assert (!pre_parse_);
      attributes r (move (attributes_.top ()));
      attributes_.pop ();
      return r;
    }

    attributes&
    attributes_top () {return attributes_.top ();}

    // Source a stream optionnaly entering it as a buildfile and performing
    // the default target processing.
    //
    void
    source (istream&,
            const path&,
            const location&,
            bool enter,
            bool default_target);

    // If chunk is true, then parse the smallest but complete, name-wise,
    // chunk of input. Note that in this case you may still end up with
    // multiple names, for example, {foo bar} or $foo. In the pre-parse mode
    // always return empty list of names.
    //
    // The what argument is used in diagnostics (e.g., "expected <what>
    // instead of ...".
    //
    // The separators argument specifies the special characters to recognize
    // inside the name. These can be the directory separators and the '%'
    // project separator. Note that even if it is NULL, the result may still
    // contain non-simple names due to variable expansions.
    //

    static const string name_separators;

    names
    parse_names (token& t, token_type& tt,
                 pattern_mode pmode,
                 bool chunk = false,
                 const char* what = "name",
                 const string* separators = &name_separators)
    {
      names ns;
      parse_names (t, tt,
                   ns,
                   pmode,
                   chunk,
                   what,
                   separators,
                   0,
                   nullopt, nullptr, nullptr);
      return ns;
    }

    // As above but return the result as a value, which can be typed and NULL.
    //
    value
    parse_value (token& t, token_type& tt,
                 pattern_mode pmode,
                 const char* what = "name",
                 const string* separators = &name_separators,
                 bool chunk = false)
    {
      names ns;
      auto r (parse_names (t, tt,
                           ns,
                           pmode,
                           chunk,
                           what,
                           separators,
                           0,
                           nullopt, nullptr, nullptr));

      value v (r.type); // Potentially typed NULL value.

      // This should not fail since we are typing the result of reversal from
      // the typed value.
      //
      if (r.not_null)
        v.assign (move (ns), nullptr);

      return v;
    }

    // Append names and return the indication if the parsed value is not NULL
    // and whether it is typed (and whether it is a pattern if pattern_mode is
    // detect).
    //
    // You may have noticed that what we return here is essentially a value
    // and doing it this way (i.e., reversing it to untyped names and
    // returning its type so that it can potentially be "typed back") is kind
    // of backwards. The reason we are doing it this way is because in many
    // places we expect things untyped and if we were to always return a
    // (potentially typed) value, then we would have to reverse it in all
    // those places. Still it may make sense to look into redesigning the
    // whole thing one day.
    //
    // Currently the only way for the result to be NULL or have a type is if
    // it is the result of a sole, unquoted variable expansion, function call,
    // or context evaluation.
    //
    struct parse_names_result
    {
      bool not_null;
      const value_type* type;
      optional<const target_type*> pattern;
    };

    parse_names_result
    parse_names (token&, token_type&,
                 names&,
                 pattern_mode,
                 bool chunk = false,
                 const char* what = "name",
                 const string* separators = &name_separators,
                 size_t pairn = 0,
                 const optional<project_name>& prj = nullopt,
                 const dir_path* dir = nullptr,
                 const string* type = nullptr,
                 bool cross = true,
                 bool curly = false);

    size_t
    parse_names_trailer (token&, token_type&,
                         names&,
                         pattern_mode,
                         const char* what,
                         const string* separators,
                         size_t pairn,
                         const optional<project_name>& prj,
                         const dir_path* dir,
                         const string* type,
                         bool cross);

    size_t
    expand_name_pattern (const location&,
                         names&&,
                         names&,
                         const char* what,
                         size_t pairn,
                         const dir_path* dir,
                         const string* type,
                         const target_type*);

    size_t
    splice_names (const location&,
                  const names_view&,
                  names&&,
                  names&,
                  const char* what,
                  size_t pairn,
                  const optional<project_name>& prj,
                  const dir_path* dir,
                  const string* type);

    // Skip until newline or eos.
    //
    void
    skip_line (token&, token_type&);

    // Skip until block-closing } or eos, taking into account nested blocks.
    //
    void
    skip_block (token&, token_type&);

    // Return true if the name token can be considered a directive keyword.
    //
    bool
    keyword (token&);

    // Buildspec.
    //
    buildspec
    parse_buildspec_clause (token&, token_type&, size_t);

    // Customization hooks.
    //
  protected:
    // If qual is not empty, then its pair member should indicate the kind
    // of qualification: ':' -- target, '/' -- scope.
    //
    virtual lookup
    lookup_variable (name&& qual, string&& name, const location&);

    // Utilities.
    //
  protected:
    class enter_scope;
    class enter_target;
    class enter_prerequisite;

    // Switch to a new current scope. Note that this function might also have
    // to switch to a new root scope if the new current scope is in another
    // project. So both must be saved and restored.
    //
    void
    switch_scope (const dir_path&);

    void
    process_default_target (token&);

    // Enter buildfile as a target.
    //
    void
    enter_buildfile (const path&);

    // Lexer.
    //
  protected:
    location
    get_location (const token& t) const
    {
      return build2::get_location (t, *path_);
    }

    token_type
    next (token&, token_type&);

    // Be careful with peeking and switching the lexer mode. See keyword()
    // for more information.
    //
    token_type
    peek ();

    token_type
    peek (lexer_mode m, char ps = '\0')
    {
      // The idea is that if we already have something peeked, then it should
      // be in the same mode. We also don't re-set the mode since it may have
      // expired after the first token.
      //
      if (peeked_)
      {
        assert (peek_.mode == m);
        return peek_.token.type;
      }

      mode (m, ps);
      return peek ();
    }

    const token&
    peeked () const
    {
      assert (peeked_);
      return peek_.token;
    }

    void
    mode (lexer_mode m, char ps = '\0')
    {
      if (replay_ != replay::play)
        lexer_->mode (m, ps);
      else
        // As a sanity check, make sure the mode matches the next token. Note
        // that we don't check the pair separator since it can be overriden by
        // the lexer's mode() implementation.
        //
        assert (replay_i_ != replay_data_.size () &&
                replay_data_[replay_i_].mode == m);
    }

    lexer_mode
    mode () const
    {
      if (replay_ != replay::play)
        return lexer_->mode ();
      else
      {
        assert (replay_i_ != replay_data_.size ());
        return replay_data_[replay_i_].mode;
      }
    }

    void
    expire_mode ()
    {
      if (replay_ != replay::play)
        lexer_->expire_mode ();
    }

    // Token saving and replaying. Note that it can only be used in certain
    // contexts. Specifically, the code that parses a replay must not interact
    // with the lexer directly (e.g., the keyword() test). Replays also cannot
    // nest. For now we don't enforce any of this.
    //
    // Note also that the peeked token is not part of the replay, until it
    // is "got".
    //
    void
    replay_save ()
    {
      assert (replay_ == replay::stop);
      replay_ = replay::save;
    }

    void
    replay_play ()
    {
      assert ((replay_ == replay::save && !replay_data_.empty ()) ||
              (replay_ == replay::play && replay_i_ == replay_data_.size ()));

      if (replay_ == replay::save)
        replay_path_ = path_; // Save old path.

      replay_i_ = 0;
      replay_ = replay::play;
    }

    void
    replay_stop ()
    {
      if (replay_ == replay::play)
        path_ = replay_path_; // Restore old path.

      replay_data_.clear ();
      replay_ = replay::stop;
    }

    struct replay_guard
    {
      replay_guard (parser& p, bool start = true)
          : p_ (start ? &p : nullptr)
      {
        if (p_ != nullptr)
          p_->replay_save ();
      }

      void
      play ()
      {
        if (p_ != nullptr)
          p_->replay_play ();
      }

      ~replay_guard ()
      {
        if (p_ != nullptr)
          p_->replay_stop ();
      }

    private:
      parser* p_;
    };

    // Stop saving and get the data.
    //
    replay_tokens
    replay_data ()
    {
      assert (replay_ == replay::save);

      replay_tokens r (move (replay_data_));
      replay_data_.clear ();
      replay_ = replay::stop;
      return r;
    }

    // Set the data and start playing.
    //
    void
    replay_data (replay_tokens&& d)
    {
      assert (replay_ == replay::stop);

      replay_path_ = path_; // Save old path.

      replay_data_ = move (d);
      replay_i_ = 0;
      replay_ = replay::play;
    }

    // Implementation details, don't call directly.
    //
    replay_token
    lexer_next ()
    {
      lexer_mode m (lexer_->mode ()); // Get it first since it may expire.
      return replay_token {lexer_->next (), path_, m};
    }

    const replay_token&
    replay_next ()
    {
      assert (replay_i_ != replay_data_.size ());
      const replay_token& rt (replay_data_[replay_i_++]);

      // Update the path. Note that theoretically it is possible that peeking
      // at the next token will "change" the path of the current token. The
      // workaround would be to call get_location() before peeking.
      //
      path_ = rt.file;

      return rt;
    }

    // Diagnostics.
    //
  protected:
    const fail_mark fail;

  protected:
    bool pre_parse_ = false;
    bool boot_;

    const path* path_; // Current path.
    lexer* lexer_;
    prerequisite* prerequisite_; // Current prerequisite, if any.
    target* target_;             // Current target, if any.
    scope* scope_;               // Current base scope (out_base).
    scope* root_;                // Current root scope (out_root).

    const dir_path* pbase_ = nullptr; // Current pattern base directory.

    std::stack<attributes> attributes_;

    target* default_target_;
    names export_value_;

    replay_token peek_;
    bool peeked_ = false;

    enum class replay {stop, save, play} replay_ = replay::stop;
    replay_tokens replay_data_;
    size_t replay_i_;         // Position of the next token during replay.
    const path* replay_path_; // Path before replay began (to be restored).
  };
}

#endif // BUILD2_PARSER_HXX
