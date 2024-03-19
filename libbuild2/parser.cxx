// file      : libbuild2/parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/parser.hxx>

#include <sstream>
#include <iostream> // cout

#include <libbutl/filesystem.hxx>   // path_search

#include <libbuild2/rule.hxx>
#include <libbuild2/dump.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/module.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite.hxx>

#include <libbuild2/adhoc-rule-cxx.hxx>
#include <libbuild2/adhoc-rule-buildscript.hxx>

#include <libbuild2/adhoc-rule-regex-pattern.hxx>

#include <libbuild2/dist/module.hxx> // module

#include <libbuild2/config/utility.hxx> // lookup_config

using namespace std;
using namespace butl;

namespace build2
{
  using type = token_type;

  ostream&
  operator<< (ostream& o, const attribute& a)
  {
    o << a.name;

    if (!a.value.null)
    {
      o << '=';
      names storage;
      to_stream (o,
                 reverse (a.value, storage, true /* reduce */),
                 quote_mode::normal,
                 '@');
    }

    return o;
  }

  class parser::enter_scope
  {
  public:
    enter_scope ()
        : p_ (nullptr), r_ (nullptr), s_ (nullptr), b_ (nullptr) {}

    enter_scope (parser& p, dir_path&& d)
        : p_ (&p), r_ (p.root_), s_ (p.scope_), b_ (p.pbase_)
    {
      complete_normalize (*p.scope_, d);
      e_ = p.switch_scope (d);
    }

    // As above but for already absolute and normalized directory.
    //
    enter_scope (parser& p, const dir_path& d, bool)
        : p_ (&p), r_ (p.root_), s_ (p.scope_), b_ (p.pbase_)
    {
      e_ = p.switch_scope (d);
    }

    ~enter_scope ()
    {
      if (p_ != nullptr)
      {
        p_->scope_ = s_;
        p_->root_ = r_;
        p_->pbase_ = b_;
      }
    }

    explicit operator bool () const {return p_ != nullptr;}

    // Note: move-assignable to empty only.
    //
    enter_scope (enter_scope&& x) noexcept {*this = move (x);}
    enter_scope& operator= (enter_scope&& x) noexcept
    {
      if (this != &x)
      {
        p_ = x.p_;
        r_ = x.r_;
        s_ = x.s_;
        b_ = x.b_;
        e_ = move (x.e_);
        x.p_ = nullptr;
      }
      return *this;
    }

    enter_scope (const enter_scope&) = delete;
    enter_scope& operator= (const enter_scope&) = delete;

    static void
    complete_normalize (scope& s, dir_path& d)
    {
      // Try hard not to call normalize(). Most of the time we will go just
      // one level deeper.
      //
      bool n (true);

      if (d.relative ())
      {
        // Relative scopes are opened relative to out, not src.
        //
        if (d.simple () && !d.current () && !d.parent ())
        {
          d = dir_path (s.out_path ()) /= d.string ();
          n = false;
        }
        else
          d = s.out_path () / d;
      }

      if (n)
        d.normalize ();
    }

  private:
    parser* p_;
    scope* r_;
    scope* s_;
    const dir_path* b_; // Pattern base.
    auto_project_env e_;
  };

  class parser::enter_target
  {
  public:
    enter_target (): p_ (nullptr), t_ (nullptr) {}

    enter_target (parser& p, target& t)
        : p_ (&p), t_ (p.target_)
    {
      p.target_ = &t;
    }

    enter_target (parser& p,
                  name&& n,  // If n.pair, then o is out dir.
                  name&& o,
                  bool implied,
                  const location& loc,
                  tracer& tr)
        : p_ (&p), t_ (p.target_)
    {
      p.target_ = &insert_target (p, move (n), move (o), implied, loc, tr);
    }

    // Find or insert.
    //
    static target&
    insert_target (parser& p,
                   name&& n,  // If n.pair, then o is out dir.
                   name&& o,
                   bool implied,
                   const location& loc,
                   tracer& tr)
    {
      auto r (p.scope_->find_target_type (n, o, loc));

      if (r.first.factory == nullptr)
        p.fail (loc) << "abstract target type " << r.first.name << "{}";

      return p.ctx->targets.insert (
        r.first,         // target type
        move (n.dir),
        move (o.dir),
        move (n.value),
        move (r.second), // extension
        implied ? target_decl::implied : target_decl::real,
        tr).first;
    }

    // Only find.
    //
    static const target*
    find_target (parser& p,
                 name& n,  // If n.pair, then o is out dir.
                 name& o,
                 const location& loc,
                 tracer& tr)
    {
      auto r (p.scope_->find_target_type (n, o, loc));

      if (r.first.factory == nullptr)
        p.fail (loc) << "abstract target type " << r.first.name << "{}";

      return p.ctx->targets.find (r.first,  // target type
                                  n.dir,
                                  o.dir,
                                  n.value,
                                  r.second, // extension
                                  tr);
    }

    ~enter_target ()
    {
      if (p_ != nullptr)
        p_->target_ = t_;
    }

    // Note: move-assignable to empty only.
    //
    enter_target (enter_target&& x) noexcept {*this = move (x);}
    enter_target& operator= (enter_target&& x) noexcept {
      p_ = x.p_; t_ = x.t_; x.p_ = nullptr; return *this;}

    enter_target (const enter_target&) = delete;
    enter_target& operator= (const enter_target&) = delete;

  private:
    parser* p_;
    target* t_;
  };

  class parser::enter_prerequisite
  {
  public:
    enter_prerequisite (): p_ (nullptr), r_ (nullptr) {}

    enter_prerequisite (parser& p, prerequisite& r)
        : p_ (&p), r_ (p.prerequisite_)
    {
      assert (p.target_ != nullptr);
      p.prerequisite_ = &r;
    }

    ~enter_prerequisite ()
    {
      if (p_ != nullptr)
        p_->prerequisite_ = r_;
    }

    // Note: move-assignable to empty only.
    //
    enter_prerequisite (enter_prerequisite&& x) noexcept {*this = move (x);}
    enter_prerequisite& operator= (enter_prerequisite&& x) noexcept {
      p_ = x.p_; r_ = x.r_; x.p_ = nullptr; return *this;}

    enter_prerequisite (const enter_prerequisite&) = delete;
    enter_prerequisite& operator= (const enter_prerequisite&) = delete;

  private:
    parser* p_;
    prerequisite* r_;
  };

  void parser::
  reset ()
  {
    pre_parse_ = false;
    attributes_.clear ();
    condition_ = nullopt;
    default_target_ = nullptr;
    peeked_ = false;
    replay_ = replay::stop;
    replay_data_.clear ();
  }

  void parser::
  parse_buildfile (istream& is,
                   const path_name& in,
                   scope* root,
                   scope& base,
                   target* tgt,
                   prerequisite* prq,
                   bool enter)
  {
    lexer l (is, in);
    parse_buildfile (l, root, base, tgt, prq, enter);
  }

  void parser::
  parse_buildfile (lexer& l,
                   scope* root,
                   scope& base,
                   target* tgt,
                   prerequisite* prq,
                   bool enter)
  {
    path_ = &l.name ();
    lexer_ = &l;

    root_ = root;
    scope_ = &base;
    target_ = tgt;
    prerequisite_ = prq;

    pbase_ = scope_->src_path_;

    // Note that root_ may not be a project root (see parse_export_stub()).
    //
    auto_project_env penv (
      stage_ != stage::boot && root_ != nullptr && root_->root_extra != nullptr
      ? auto_project_env (*root_)
      : auto_project_env ());

    const buildfile* bf (enter && path_->path != nullptr
                         ? &enter_buildfile<buildfile> (*path_->path)
                         : nullptr);
    token t;
    type tt;
    next (t, tt);

    if (target_ != nullptr || prerequisite_ != nullptr)
    {
      parse_variable_block (t, tt);
    }
    else
    {
      parse_clause (t, tt);

      if (stage_ != stage::boot && stage_ != stage::root)
        process_default_target (t, bf);
    }

    if (tt != type::eos)
      fail (t) << "unexpected " << t;
  }

  names parser::
  parse_export_stub (istream& is, const path_name& name,
                     const scope& rs, scope& gs, scope& ts)
  {
    // Enter the export stub manually with correct out.
    //
    if (name.path != nullptr)
    {
      dir_path out (!rs.out_eq_src ()
                    ? out_src (name.path->directory (), rs)
                    : dir_path ());

      enter_buildfile<buildfile> (*name.path, move (out));
    }

    parse_buildfile (is, name, &gs, ts, nullptr, nullptr, false /* enter */);
    return move (export_value);
  }

  token parser::
  parse_variable (lexer& l, scope& s, const variable& var, type kind)
  {
    path_ = &l.name ();
    lexer_ = &l;

    root_ = nullptr;
    scope_ = &s;
    target_ = nullptr;
    prerequisite_ = nullptr;

    pbase_ = scope_->src_path_; // Normally NULL.

    token t;
    type tt;
    parse_variable (t, tt, var, kind);
    return t;
  }

  pair<value, token> parser::
  parse_variable_value (lexer& l,
                        scope& s,
                        const dir_path* b,
                        const variable& var)
  {
    path_ = &l.name ();
    lexer_ = &l;

    root_ = nullptr;
    scope_ = &s;
    target_ = nullptr;
    prerequisite_ = nullptr;

    pbase_ = b;

    token t;
    type tt;
    value rhs (parse_variable_value (t, tt));

    value lhs;
    apply_value_attributes (&var, lhs, move (rhs), type::assign);

    return make_pair (move (lhs), move (t));
  }

  names parser::
  parse_names (lexer& l,
               const dir_path* b,
               pattern_mode pmode,
               const char* what,
               const string* separators)
  {
    path_ = &l.name ();
    lexer_ = &l;

    root_ = nullptr;
    scope_ = nullptr;
    target_ = nullptr;
    prerequisite_ = nullptr;

    pbase_ = b;

    token t;
    type tt;

    mode (lexer_mode::value, '@');
    next (t, tt);

    names r (parse_names (t, tt, pmode, what, separators));

    if (tt != type::eos)
      fail (t) << "unexpected " << t;

    return r;
  }

  value parser::
  parse_eval (lexer& l, scope& rs, scope& bs, pattern_mode pmode)
  {
    path_ = &l.name ();
    lexer_ = &l;

    root_ = &rs;
    scope_ = &bs;
    target_ = nullptr;
    prerequisite_ = nullptr;

    pbase_ = scope_->src_path_;

    // Note that root_ may not be a project root.
    //
    auto_project_env penv (
      stage_ != stage::boot && root_ != nullptr && root_->root_extra != nullptr
      ? auto_project_env (*root_)
      : auto_project_env ());

    token t;
    type tt;
    next (t, tt);

    if (tt != type::lparen)
      fail (t) << "expected '(' instead of " << t;

    location loc (get_location (t));
    mode (lexer_mode::eval, '@');
    next_with_attributes (t, tt);

    values vs (parse_eval (t, tt, pmode));

    if (next (t, tt) != type::eos)
      fail (t) << "unexpected " << t;

    switch (vs.size ())
    {
    case 0:  return value (names ());
    case 1:  return move (vs[0]);
    default: fail (loc) << "expected single value" << endf;
    }
  }

  bool parser::
  parse_clause (token& t, type& tt, bool one)
  {
    tracer trace ("parser::parse_clause", &path_);

    // This function should be called in the normal lexing mode with the first
    // token of a line or an alternative arrangements may have to be made to
    // recognize the attributes.
    //
    // It should also always stop at a token that is at the beginning of the
    // line (except for eof). That is, if something is called to parse a line,
    // it should parse it until newline (or fail). This is important for
    // if-else blocks, directory scopes, etc., that assume the '}' token they
    // see is on the new line.
    //
    bool parsed (false);

    while (tt != type::eos && !(one && parsed))
    {
      // Issue better diagnostics for stray `%`.
      //
      if (tt == type::percent)
        fail (t) << "recipe without target";

      // Extract attributes if any.
      //
      assert (attributes_.empty ());
      auto at (attributes_push (t, tt));

      // We always start with one or more names, potentially <>-grouped.
      //
      if (!(start_names (tt) || tt == type::labrace))
      {
        // Something else. Let our caller handle that.
        //
        if (at.first)
          fail (at.second) << "attributes before " << t;
        else
          attributes_pop ();

        break;
      }

      // Now we will either parse something or fail.
      //
      if (!parsed)
        parsed = true;

      // See if this is one of the directives.
      //
      if (tt == type::word && keyword (t))
      {
        const string& n (t.value);
        void (parser::*f) (token&, type&) = nullptr;

        // @@ Is this the only place where some of these are valid? Probably
        // also in the var block?
        //
        if (n == "assert" ||
            n == "assert!")
        {
          f = &parser::parse_assert;
        }
        else if (n == "print") // Unlike text goes to stdout.
        {
          f = &parser::parse_print;
        }
        else if (n == "fail"  ||
                 n == "warn"  ||
                 n == "info"  ||
                 n == "text")
        {
          f = &parser::parse_diag;
        }
        else if (n == "dump")
        {
          f = &parser::parse_dump;
        }
        else if (n == "source")
        {
          f = &parser::parse_source;
        }
        else if (n == "include")
        {
          f = &parser::parse_include;
        }
        else if (n == "run")
        {
          f = &parser::parse_run;
        }
        else if (n == "import"  ||
                 n == "import?" ||
                 n == "import!")
        {
          f = &parser::parse_import;
        }
        else if (n == "export")
        {
          f = &parser::parse_export;
        }
        else if (n == "using" ||
                 n == "using?")
        {
          f = &parser::parse_using;
        }
        else if (n == "define")
        {
          f = &parser::parse_define;
        }
        else if (n == "if" ||
                 n == "if!")
        {
          f = &parser::parse_if_else;
        }
        else if (n == "else" ||
                 n == "elif" ||
                 n == "elif!")
        {
          // Valid ones are handled in parse_if_else().
          //
          fail (t) << n << " without if";
        }
        else if (n == "switch")
        {
          f = &parser::parse_switch;
        }
        else if (n == "case"  ||
                 n == "default")
        {
          // Valid ones are handled in parse_switch().
          //
          fail (t) << n << " outside switch";
        }
        else if (n == "for")
        {
          f = &parser::parse_for;
        }
        else if (n == "config")
        {
          f = &parser::parse_config;
        }
        else if (n == "config.environment")
        {
          f = &parser::parse_config_environment;
        }
        else if (n == "recipe")
        {
          // Valid only after recipe header (%).
          //
          fail (t) << n << " directive without % recipe header";
        }

        if (f != nullptr)
        {
          if (at.first)
            fail (at.second) << "attributes before " << n;
          else
            attributes_pop ();

          (this->*f) (t, tt);
          continue;
        }
      }

      location nloc (get_location (t));
      names ns;

      // We have to parse names in chunks to detect invalid cases of the
      // group{foo}<...> syntax.
      //
      // Consider (1):
      //
      // x =
      // group{foo} $x<...>:
      //
      // And (2):
      //
      // x = group{foo} group{bar}
      // $x<...>:
      //
      // As well as (3):
      //
      // <...><...>:
      //
      struct chunk
      {
        size_t   pos; // Index in ns of the beginning of the last chunk.
        location loc; // Position of the beginning of the last chunk.
      };
      optional<chunk> ns_last;

      bool labrace_first (tt == type::labrace);
      if (!labrace_first)
      {
        do
        {
          ns_last = chunk {ns.size (), get_location (t)};
          parse_names (t, tt, ns, pattern_mode::preserve, true /* chunk */);
        }
        while (start_names (tt));

        // Allow things like function calls that don't result in anything.
        //
        if (tt == type::newline && ns.empty ())
        {
          if (at.first)
            fail (at.second) << "standalone attributes";
          else
            attributes_pop ();

          next (t, tt);
          continue;
        }
      }

      // Handle target group specification (<...>).
      //
      // We keep an "optional" (empty) vector of names parallel to ns that
      // contains the group members. Note that when we "catch" gns up to ns,
      // we populate it with ad hoc (as opposed to explicit) groups with no
      // members.
      //
      group_names gns;
      if (tt == type::labrace)
      {
        for (; tt == type::labrace; labrace_first = false)
        {
          // Detect explicit group (group{foo}<...>).
          //
          // Note that `<` first thing on the line is not seperated thus the
          // labrace_first complication.
          //
          bool expl (!t.separated && !labrace_first);
          if (expl)
          {
            // Note: (N) refers to the example in the above comment.
            //
            if (!ns_last /* (3) */ || ns_last->pos == ns.size () /* (1) */)
            {
              fail (t) << "group name or whitespace expected before '<'";
            }
            else
            {
              size_t n (ns.size () - ns_last->pos);

              // Note: could be a pair.
              //
              if ((n > 2 || (n == 2 && !ns[ns_last->pos].pair)) /* (2) */)
              {
                fail (t) << "single group name or whitespace expected before "
                         << "'<' instead of '"
                         << names_view (ns.data () + ns_last->pos, n) << "'";
              }
            }
          }

          // Parse target names inside <>.
          //
          // We "reserve" the right to have attributes inside <> though what
          // exactly that would mean is unclear. One potentially useful
          // semantics would be the ability to specify attributes for group
          // members though the fact that the primary target for ad hoc groups
          // is listed first would make it rather unintuitive. Maybe
          // attributes that change the group semantics itself?
          //
          next_with_attributes (t, tt);

          auto at (attributes_push (t, tt));

          if (at.first)
            fail (at.second) << "attributes before group member";
          else
            attributes_pop ();

          // For explicit groups, the group target is already in ns and all
          // the members should go straight to gns.
          //
          // For ad hoc groups, the first name (or a pair) is the primary
          // target which we need to keep in ns. The rest, if any, are ad
          // hoc members that we should move to gns.
          //
          if (expl)
          {
            gns.resize (ns.size ()); // Catch up with the names vector.
            group_names_loc& g (gns.back ());
            g.expl = true;
            g.group_loc = move (ns_last->loc);
            g.member_loc = get_location (t); // Start of members.

            if (tt != type::rabrace) // Handle empty case (<>)
              parse_names (t, tt, g.ns, pattern_mode::preserve);
          }
          else if (tt != type::rabrace) // Allow and ignore empty case (<>).
          {
            location mloc (get_location (t)); // Start of members.

            size_t m (ns.size ());
            parse_names (t, tt, ns, pattern_mode::preserve);
            size_t n (ns.size ());

            // Another empty case (<$empty>).
            //
            if (m != n)
            {
              m = n - m - (ns[m].pair ? 2 : 1); // Number of names to move.

              // Allow degenerate case with just the primary target.
              //
              if (m != 0)
              {
                n -= m; // Number of names in ns we should end up with.

                gns.resize (n); // Catch up with the names vector.
                group_names_loc& g (gns.back ());
                g.group_loc = g.member_loc = move (mloc);
                g.ns.insert (g.ns.end (),
                             make_move_iterator (ns.begin () + n),
                             make_move_iterator (ns.end ()));
                ns.resize (n);
              }
            }
          }

          if (tt != type::rabrace)
            fail (t) << "expected '>' instead of " << t;

          // Parse the next chunk of target names after >, if any.
          //
          next (t, tt);
          ns_last = nullopt; // To detect <...><...>.
          while (start_names (tt))
          {
            ns_last = chunk {ns.size (), get_location (t)};
            parse_names (t, tt, ns, pattern_mode::preserve, true /* chunk */);
          }
        }

        if (!gns.empty ())
          gns.resize (ns.size ()); // Catch up with the final chunk.

        if (tt != type::colon)
          fail (t) << "expected ':' instead of " << t;

        if (ns.empty ())
          fail (t) << "expected target before ':'";
      }

      // If we have a colon, then this is target-related.
      //
      if (tt == type::colon)
      {
        // While '{}:' means empty name, '{$x}:' where x is empty list
        // means empty list.
        //
        if (ns.empty ())
          fail (t) << "expected target before ':'";

        attributes as (attributes_pop ());

        // Call the specified parsing function (variable value/block) for
        // one/each pattern/target. We handle multiple targets by replaying
        // the tokens since the value/block may contain variable expansions
        // that would be sensitive to the target context in which they are
        // evaluated. The function signature is:
        //
        // void (token& t, type& tt,
        //       optional<bool> member, // true -- explict, false -- ad hoc
        //       optional<pattern_type>, const target_type* pat_tt, string pat,
        //       const location& pat_loc)
        //
        // Note that the target and its group members are inserted implied
        // but this flag can be cleared and default_target logic applied if
        // appropriate.
        //
        auto for_one_pat = [this, &t, &tt] (auto&& f,
                                            name&& n,
                                            const location& nloc)
        {
          // Reduce the various directory/value combinations to the scope
          // directory (if any) and the pattern. Here are more interesting
          // examples of patterns:
          //
          // */           --  */{}
          // dir{*}       --  dir{*}
          // dir{*/}      --  */dir{}
          //
          // foo/*/       --  foo/*/{}
          // foo/dir{*/}  --  foo/*/dir{}
          //
          // Note that these are not patterns:
          //
          // foo*/file{bar}
          // foo*/dir{bar/}
          //
          // While these are:
          //
          // file{foo*/bar}
          // dir{foo*/bar/}
          //
          // And this is a half-pattern (foo* should no be treated as a
          // pattern but that's unfortunately indistinguishable):
          //
          // foo*/dir{*/}  --  foo*/*/dir{}
          //
          // Note also that none of this applies to regex patterns (see
          // the parsing code for details).
          //
          if (*n.pattern == pattern_type::path)
          {
            if (n.value.empty () && !n.dir.empty ())
            {
              // Note that we use string and not the representation: in a
              // sense the trailing slash in the pattern is subsumed by
              // the target type.
              //
              if (n.dir.simple ())
                n.value = move (n.dir).string ();
              else
              {
                n.value = n.dir.leaf ().string ();
                n.dir.make_directory ();
              }

              // Treat directory as type dir{} similar to other places.
              //
              if (n.untyped ())
                n.type = "dir";
            }
            else
            {
              // Move the directory part, if any, from value to dir.
              //
              try
              {
                n.canonicalize ();
              }
              catch (const invalid_path& e)
              {
                fail (nloc) << "invalid path '" << e.path << "'";
              }
              catch (const invalid_argument&)
              {
                fail (nloc) << "invalid pattern '" << n.value << "'";
              }
            }
          }

          // If we have the directory, then it is the scope.
          //
          enter_scope sg;
          if (!n.dir.empty ())
          {
            if (path_pattern (n.dir))
              fail (nloc) << "pattern in directory " << n.dir.representation ();

            sg = enter_scope (*this, move (n.dir));
          }

          // Resolve target type. If none is specified, then it's file{}.
          //
          // Note: abstract target type is ok here.
          //
          const target_type* ttype (n.untyped ()
                                    ? &file::static_type
                                    : scope_->find_target_type (n.type));

          if (ttype == nullptr)
            fail (nloc) << "unknown target type " << n.type <<
              info << "perhaps the module that defines this target type is "
                   << "not loaded by project " << *scope_->root_scope ();

          f (t, tt, nullopt, n.pattern, ttype, move (n.value), nloc);
        };

        auto for_each = [this, &trace, &for_one_pat,
                         &t, &tt, &as, &ns, &nloc, &gns] (auto&& f)
        {
          // We need replay if we have multiple targets or group members.
          //
          // Note: watch out for an out-qualified single target (two names).
          //
          replay_guard rg (*this,
                           ns.size () > 2 ||
                           (ns.size () == 2 && !ns[0].pair) ||
                           !gns.empty ());

          for (size_t i (0), e (ns.size ()); i != e; )
          {
            name& n (ns[i]);

            if (n.qualified ())
              fail (nloc) << "project name in target " << n;

            // Figure out if this is a target or a target type/pattern (yeah,
            // it can be a mixture).
            //
            if (n.pattern)
            {
              if (!as.empty ())
                fail (as.loc) << "attributes before target type/pattern";

              if (n.pair)
                fail (nloc) << "out-qualified target type/pattern";

              if (!gns.empty () && !gns[i].ns.empty ())
                fail (gns[i].member_loc)
                  << "group member in target type/pattern";

              if (*n.pattern == pattern_type::regex_substitution)
                fail (nloc) << "regex substitution " << n << " without "
                            << "regex pattern";

              for_one_pat (forward<decltype (f)> (f), move (n), nloc);
            }
            else
            {
              bool expl;
              vector<reference_wrapper<target>> gms;
              {
                name o (n.pair ? move (ns[++i]) : name ());
                enter_target tg (*this,
                                 move (n),
                                 move (o),
                                 true /* implied */,
                                 nloc,
                                 trace);

                if (!as.empty ())
                  apply_target_attributes (*target_, as);

                // Enter group members.
                //
                if (!gns.empty ())
                {
                  // Note: index after the pair increment.
                  //
                  group_names_loc& g (gns[i]);
                  expl = g.expl;

                  if (expl && !target_->is_a<group> ())
                    fail (g.group_loc) << *target_ << " is not group target";

                  gms = expl
                    ? enter_explicit_members (move (g), true /* implied */)
                    : enter_adhoc_members (move (g), true /* implied */);
                }

                f (t, tt, nullopt, nullopt, nullptr, string (), location ());
              }

              for (target& gm: gms)
              {
                rg.play (); // Replay.

                enter_target tg (*this, gm);
                f (t, tt, expl, nullopt, nullptr, string (), location ());
              }
            }

            if (++i != e)
              rg.play (); // Replay.
          }
        };

        next_with_attributes (t, tt); // Recognize attributes after `:`.

        // See if this could be an ad hoc pattern rule. It's a pattern rule if
        // the primary target is a pattern and it has (1) prerequisites and/or
        // (2) recipes. Only one primary target per pattern rule declaration
        // is allowed.
        //
        // Note, however, that what looks like a pattern may turn out to be
        // just a pattern-specific variable assignment or variable block,
        // which both can appear with multiple targets/patterns on the left
        // hand side, or even a mixture of them. Still, instead of trying to
        // weave the pattern rule logic into the already hairy code below, we
        // are going to handle it separately and deal with the "degenerate"
        // cases (variable assignment/block) both here and below.
        //
        if (ns[0].pattern && ns.size () == (ns[0].pair ? 2 : 1))
        {
          name& n (ns[0]);

          if (n.qualified ())
            fail (nloc) << "project name in target pattern " << n;

          if (n.pair)
            fail (nloc) << "out-qualified target pattern";

          if (*n.pattern == pattern_type::regex_substitution)
            fail (nloc) << "regex substitution " << n << " without "
                        << "regex pattern";

          // Parse prerequisites, if any.
          //
          location ploc;
          names pns;
          if (tt != type::newline)
          {
            auto at (attributes_push (t, tt));

            if (!start_names (tt))
              fail (t) << "unexpected " << t;

            // Note that unlike below, here we preserve the pattern in the
            // prerequisites.
            //
            ploc = get_location (t);
            pns = parse_names (t, tt, pattern_mode::preserve);

            // Target type/pattern-specific variable assignment.
            //
            if (tt == type::assign || tt == type::prepend || tt == type::append)
            {
              // Note: ns contains single target name.
              //
              if (!gns.empty ())
                fail (gns[0].member_loc)
                  << "group member in target type/pattern";

              // Note: see the same code below if changing anything here.
              //
              type akind (tt);
              const location aloc (get_location (t));

              const variable& var (parse_variable_name (move (pns), ploc));
              apply_variable_attributes (var);

              if (var.visibility > variable_visibility::target)
              {
                fail (nloc) << "variable " << var << " has " << var.visibility
                            << " visibility but is assigned on a target";
              }

              for_one_pat (
                [this, &var, akind, &aloc] (
                  token& t, type& tt,
                  optional<bool>,
                  optional<pattern_type> pt, const target_type* ptt,
                  string pat, const location& ploc)
                {

                  parse_type_pattern_variable (t, tt,
                                               *pt, *ptt, move (pat), ploc,
                                               var, akind, aloc);
                },
                move (n),
                nloc);

              next_after_newline (t, tt);

              if (!as.empty ())
                fail (as.loc) << "attributes before target type/pattern";

              continue; // Just a target type/pattern-specific var assignment.
            }

            if (at.first)
              fail (at.second) << "attributes before prerequisite pattern";
            else
              attributes_pop ();

            // @@ TODO
            //
            if (tt == type::colon)
              fail (t) << "prerequisite type/pattern-specific variables "
                       << "not yet supported";
          }

          // Next we may have a target type/pattern specific variable block
          // potentially followed by recipes.
          //
          next_after_newline (t, tt);
          if (tt == type::lcbrace && peek () == type::newline)
          {
            // Note: see the same code below if changing anything here.
            //
            next (t, tt); // Newline.
            next (t, tt); // First token inside the variable block.

            for_one_pat (
              [this] (
                token& t, type& tt,
                optional<bool>,
                optional<pattern_type> pt, const target_type* ptt,
                string pat, const location& ploc)
              {
                parse_variable_block (t, tt, pt, ptt, move (pat), ploc);
              },
              name (n), // Note: can't move (could still be a rule).
              nloc);

            if (tt != type::rcbrace)
              fail (t) << "expected '}' instead of " << t;

            next (t, tt);                    // Newline.
            next_after_newline (t, tt, '}'); // Should be on its own line.

            // See if this is just a target type/pattern-specific var block.
            //
            if (pns.empty () &&
                tt != type::percent && tt != type::multi_lcbrace)
            {
              // Note: ns contains single target name.
              //
              if (!gns.empty ())
                fail (gns[0].member_loc)
                  << "group member in target type/pattern";

              if (!as.empty ())
                fail (as.loc) << "attributes before target type/pattern";

              continue;
            }
          }

          // Ok, this is an ad hoc pattern rule.
          //
          // First process the attributes.
          //
          string rn;
          {
            const location& l (as.loc);

            for (auto& a: as)
            {
              const string& n (a.name);
              value& v (a.value);

              // rule_name=
              //
              if (n == "rule_name")
              {
                try
                {
                  rn = convert<string> (move (v));

                  if (rn.empty ())
                    throw invalid_argument ("empty name");
                }
                catch (const invalid_argument& e)
                {
                  fail (l) << "invalid " << n << " attribute value: " << e;
                }
              }
              else
                fail (l) << "unknown ad hoc pattern rule attribute " << a;
            }
          }

          // What should we do if we have neither prerequisites nor recipes?
          // While such a declaration doesn't make much sense, it can happen,
          // for example, with an empty variable expansion:
          //
          // file{*.txt}: $extra
          //
          // So let's silently ignore it.
          //
          if (pns.empty () && tt != type::percent && tt != type::multi_lcbrace)
            continue;

          // Process and verify the pattern.
          //
          pattern_type pt (*n.pattern);
          optional<pattern_type> st;
          const char* pn;

          switch (pt)
          {
          case pattern_type::path:
            pn = "path";
            break;
          case pattern_type::regex_pattern:
            pn = "regex";
            st = pattern_type::regex_substitution;
            break;
          case pattern_type::regex_substitution:
            // Unreachable.
            break;
          }

          // Make sure patterns have no directory components. While we may
          // decide to support this in the future, currently the appropriate
          // semantics is not immediately obvious. Whatever we decide, it
          // should be consistent with the target type/pattern-specific
          // variables where it is interpreted as a scope (and which doesn't
          // feel like the best option for pattern rules). See also depdb
          // dyndep --update-* patterns.
          //
          auto check_pattern = [this] (name& n, const location& loc)
          {
            try
            {
              // Move the directory component for path patterns.
              //
              if (*n.pattern == pattern_type::path)
                n.canonicalize ();

              if (n.dir.empty ())
                return;
            }
            catch (const invalid_path&)
            {
              // Fall through.
            }

            fail (loc) << "directory in pattern " << n;
          };

          check_pattern (n, nloc);

          // If we have group members, verify all the members are patterns or
          // substitutions (ad hoc) or subsitutions (explicit) and of the
          // correct pattern type. A rule for an explicit group that wishes to
          // match based on some of its members feels far fetched.
          //
          // For explicit groups the use-case is to inject static members
          // which could otherwise be tedious to specify for each group.
          //
          const location& mloc (gns.empty () ? location () : gns[0].member_loc);
          names ns (gns.empty () ? names () : move (gns[0].ns));
          bool expl (gns.empty () ? false : gns[0].expl);

          for (name& n: ns)
          {
            if (!n.pattern || !(*n.pattern == pt || (st && *n.pattern == *st)))
            {
              fail (mloc) << "expected " << pn << " pattern or substitution "
                          << "instead of " << n;
            }

            if (*n.pattern != pattern_type::regex_substitution)
            {
              if (expl)
                fail (mloc) << "explicit group member pattern " << n;

              check_pattern (n, mloc);
            }
          }

          // The same for prerequisites except here we can have non-patterns.
          //
          for (name& n: pns)
          {
            if (n.pattern)
            {
              if (!(*n.pattern == pt || (st && *n.pattern == *st)))
              {
                fail (ploc) << "expected " << pn << " pattern or substitution "
                            << "instead of " << n;
              }

              if (*n.pattern != pattern_type::regex_substitution)
                check_pattern (n, ploc);
            }
          }

          // Derive the rule name unless specified explicitly. It must be
          // unique in this scope.
          //
          // It would have been nice to include the location but unless we
          // include the absolute path to the buildfile (which would be
          // unwieldy), it could be ambigous.
          //
          // NOTE: we rely on the <...> format in dump.
          //
          if (rn.empty ())
            rn = "<ad hoc pattern rule #" +
              to_string (scope_->adhoc_rules.size () + 1) + '>';

          auto& ars (scope_->adhoc_rules);

          auto i (find_if (ars.begin (), ars.end (),
                           [&rn] (const unique_ptr<adhoc_rule_pattern>& rp)
                           {
                             return rp->rule_name == rn;
                           }));

          const target_type* ttype (nullptr);
          if (i != ars.end ())
          {
            // @@ TODO: append ad hoc members, prereqs (we now have
            //          [rule_name=] which we can use to reference the same
            //          rule).
            //
            ttype = &(*i)->type;
            assert (false);
          }
          else
          {
            // Resolve target type (same as in for_one_pat()).
            //
            ttype = n.untyped ()
              ? &file::static_type
              : scope_->find_target_type (n.type);

            if (ttype == nullptr)
              fail (nloc) << "unknown target type " << n.type <<
                info << "perhaps the module that defines this target type is "
                     << "not loaded by project " << *scope_->root_scope ();

            if (!gns.empty ())
            {
              if (ttype->is_a<group> () != expl)
                fail (nloc) << "group type and target type mismatch";
            }

            unique_ptr<adhoc_rule_pattern> rp;
            switch (pt)
            {
            case pattern_type::path:
              // @@ TODO
              fail (nloc) << "path pattern rules not yet supported";
              break;
            case pattern_type::regex_pattern:
              rp.reset (new adhoc_rule_regex_pattern (
                          *scope_, rn, *ttype,
                          move (n), nloc,
                          move (ns), mloc,
                          move (pns), ploc));
              break;
            case pattern_type::regex_substitution:
              // Unreachable.
              break;
            }

            ars.push_back (move (rp));
            i = --ars.end ();
          }

          adhoc_rule_pattern& rp (**i);

          // Parse the recipe chain if any.
          //
          if (tt == type::percent || tt == type::multi_lcbrace)
          {
            small_vector<shared_ptr<adhoc_rule>, 1> recipes;
            parse_recipe (t, tt, token (t), recipes, ttype, rn);

            for (shared_ptr<adhoc_rule>& pr: recipes)
            {
              // Can be NULL if the recipe is disabled with a condition.
              //
              if (pr != nullptr)
              {
                pr->pattern = &rp; // Connect recipe to pattern.
                rp.rules.push_back (move (pr));
              }
            }

            // Register this adhoc rule for all its actions.
            //
            for (shared_ptr<adhoc_rule>& pr: rp.rules)
            {
              adhoc_rule& r (*pr);

              for (action a: r.actions)
              {
                // This covers both duplicate recipe actions within the rule
                // pattern (similar to parse_recipe()) as well as conflicts
                // with other rules (ad hoc or not).
                //
                if (!scope_->rules.insert (a, *ttype, rp.rule_name, r))
                {
                  const meta_operation_info* mf (
                    root_->root_extra->meta_operations[a.meta_operation ()]);

                  const operation_info* of (
                    root_->root_extra->operations[a.operation ()]);

                  fail (r.loc)
                    << "duplicate " << mf->name << '(' << of->name << ") rule "
                    << rp.rule_name << " for target type " << ttype->name
                    << "{}";
                }

                // We also register for a wildcard operation in order to get
                // called to provide the reverse operation fallback (see
                // match_impl() for the gory details).
                //
                // Note that we may end up trying to insert a duplicate of the
                // same rule (e.g., for the same meta-operation). Feels like
                // we should never try to insert for a different rule since
                // for ad hoc rules names are unique.
                //
                scope_->rules.insert (
                  a.meta_operation (), 0,
                  *ttype, rp.rule_name, rp.fallback_rule_);

                // We also register for the dist meta-operation in order to
                // inject additional prerequisites which may "pull" additional
                // sources into the distribution. Unless there is an explicit
                // recipe for dist.
                //
                // And the same for the configure meta-operation to, for
                // example, make sure a hinted ad hoc rule matches. @@ Hm,
                // maybe we fixed this with action-specific hints? But the
                // injection part above may still apply. BTW, this is also
                // required for see-through groups in order to resolve their
                // member.
                //
                // Note also that the equivalent semantics for ad hoc recipes
                // is provided by match_adhoc_recipe().
                //
                if (a.meta_operation () == perform_id)
                {
                  auto reg = [this, ttype, &rp, &r] (action ea)
                  {
                    for (shared_ptr<adhoc_rule>& pr: rp.rules)
                    for (action a: pr->actions)
                      if (ea == a)
                        return;

                    scope_->rules.insert (ea, *ttype, rp.rule_name, r);
                  };

                  reg (action (dist_id, a.operation ()));
                  reg (action (configure_id, a.operation ()));
                }

                // @@ TODO: if this rule does dynamic member discovery of a
                //    see-through target group, then we may also need to
                //    register update for other meta-operations (see, for
                //    example, wildcard update registration in the cli
                //    module). BTW, we can now detect such a target via
                //    its target type flags.
              }
            }
          }

          continue;
        }

        if (tt == type::newline)
        {
          // See if this is a target-specific variable and/or recipe block(s).
          //
          // Note that we cannot just let parse_dependency() handle this case
          // because we can have (a mixture of) target type/patterns.
          //
          // Also, it handles the exception to the rule that if a dependency
          // declaration ends with a colon, then the variable assignment/block
          // that follows is for the prerequisite. Compare:
          //
          // foo: x = y         foo: fox: x = y
          // bar:               bar: baz:
          // {                  {
          //   x = y              x = y
          // }                  }
          //
          next (t, tt);
          if (tt == type::percent       ||
              tt == type::multi_lcbrace ||
              (tt == type::lcbrace && peek () == type::newline))
          {
            // Parse the block(s) for each target.
            //
            // Note that because we have to peek past the closing brace(s) to
            // see whether there is a/another recipe block, we have to make
            // that token part of the replay (we cannot peek past the replay
            // sequence).
            //
            // Note: similar code to the version in parse_dependency().
            //
            auto parse = [
              this,
              st = token (t), // Save start token (will be gone on replay).
              recipes = small_vector<shared_ptr<adhoc_rule>, 1> ()]
              (token& t, type& tt,
               optional<bool> gm, // true -- explicit, false -- ad hoc
               optional<pattern_type> pt, const target_type* ptt, string pat,
               const location& ploc) mutable
            {
              token rt; // Recipe start token.

              // The variable block, if any, should be first.
              //
              if (st.type == type::lcbrace)
              {
                // Note: see the same code above if changing anything here.
                //
                next (t, tt); // Newline.
                next (t, tt); // First token inside the variable block.

                // For explicit groups we only assign variables on the group
                // omitting the members.
                //
                if (!gm || !*gm)
                  parse_variable_block (t, tt, pt, ptt, move (pat), ploc);
                else
                  skip_block (t, tt);

                if (tt != type::rcbrace)
                  fail (t) << "expected '}' instead of " << t;

                next (t, tt);                    // Newline.
                next_after_newline (t, tt, '}'); // Should be on its own line.

                if (tt != type::percent && tt != type::multi_lcbrace)
                  return;

                rt = t;
              }
              else
                rt = st;

              // If this is a group member then we know we are replaying and
              // can skip the recipe.
              //
              if (gm)
              {
                replay_skip ();
                next (t, tt);
                return;
              }

              if (pt)
                fail (rt) << "unexpected recipe after target type/pattern" <<
                  info << "ad hoc pattern rule may not be combined with other "
                       << "targets or patterns";

              parse_recipe (t, tt, rt, recipes);
            };

            for_each (parse);
          }
          else
          {
            // If not followed by a block, then it's a target without any
            // prerequisites. We, however, cannot just fall through to the
            // parse_dependency() call because we have already seen the next
            // token.
            //
            // Note also that we treat this as an explicit dependency
            // declaration (i.e., not implied).
            //
            enter_targets (move (ns), nloc, move (gns), 0, as);
          }

          continue;
        }

        // Target-specific variable assignment or dependency declaration,
        // including a dependency chain and/or prerequisite-specific variable
        // assignment and/or recipe block(s).
        //
        auto at (attributes_push (t, tt));

        if (!start_names (tt))
          fail (t) << "unexpected " << t;

        // @@ PAT: currently we pattern-expand target-specific var names (see
        //         also parse_import()).
        //
        const location ploc (get_location (t));
        names pns (parse_names (t, tt, pattern_mode::expand));

        // Target-specific variable assignment.
        //
        // Note that neither here nor in parse_dependency() below we allow
        // specifying recipes following a target-specified variable assignment
        // (but we do allow them following a target-specific variable block).
        //
        if (tt == type::assign || tt == type::prepend || tt == type::append)
        {
          // Note: see the same code above if changing anything here.
          //
          type akind (tt);
          const location aloc (get_location (t));

          const variable& var (parse_variable_name (move (pns), ploc));
          apply_variable_attributes (var);

          // If variable visibility ends before, then it doesn't make sense
          // to assign it in this context.
          //
          if (var.visibility > variable_visibility::target)
          {
            fail (nloc) << "variable " << var << " has " << var.visibility
                        << " visibility but is assigned on a target";
          }

          // Parse the assignment for each target.
          //
          for_each (
            [this, &var, akind, &aloc] (
              token& t, type& tt,
              optional<bool> gm,
              optional<pattern_type> pt, const target_type* ptt, string pat,
              const location& ploc)
            {
              if (pt)
                parse_type_pattern_variable (t, tt,
                                             *pt, *ptt, move (pat), ploc,
                                             var, akind, aloc);
              else
              {
                // Skip explicit group members (see the block case above for
                // background).
                //
                if (!gm || !*gm)
                  parse_variable (t, tt, var, akind);
                else
                {
                  next (t, tt);
                  skip_line (t, tt);
                }
              }
            });

          next_after_newline (t, tt);
        }
        // Dependency declaration potentially followed by a chain and/or a
        // target/prerequisite-specific variable assignment/block and/or
        // recipe block(s).
        //
        else
        {
          if (at.first)
            fail (at.second) << "attributes before prerequisites";
          else
            attributes_pop ();

          parse_dependency (t, tt,
                            move (ns), nloc,
                            move (gns),
                            move (pns), ploc,
                            as);
        }

        continue;
      }

      // Variable assignment.
      //
      // This can take any of the following forms:
      //
      //        x = y
      //   foo/ x = y   (ns will have two elements)
      //    foo/x = y   (ns will have one element)
      //
      // And in the future we may also want to support:
      //
      //   foo/ bar/ x = y
      //
      // Note that we don't support this:
      //
      //   foo/ [attrs] x = y
      //
      // Because the meaning of `[attrs]` would be ambiguous (it could also be
      // a name). Note that the above semantics can be easily achieved with an
      // explicit directory scope:
      //
      //   foo/
      //   {
      //     [attrs] x = y
      //   }
      //
      if (tt == type::assign || tt == type::prepend || tt == type::append)
      {
        // Detect and handle the directory scope. If things look off, then we
        // let parse_variable_name() complain.
        //
        dir_path d;
        size_t p;
        if ((ns.size () == 2 && ns[0].directory ()) ||
            (ns.size () == 1 && ns[0].simple () &&
             (p = path_traits::rfind_separator (ns[0].value)) != string::npos))
        {
          if (at.first)
            fail (at.second) << "attributes before scope directory";

          // Make sure it's not a pattern (see also the target case above and
          // scope below).
          //
          if (ns[0].pattern)
            fail (nloc) << "pattern in " << ns[0];

          if (ns.size () == 2)
          {
            d = move (ns[0].dir);
            ns.erase (ns.begin ());
          }
          else
          {
            // Note that p cannot point to the last character since then it
            // would have been a directory, not a simple name.
            //
            d = dir_path (ns[0].value, 0, p + 1);
            ns[0].value.erase (0, p + 1);
          }
        }

        const variable& var (parse_variable_name (move (ns), nloc));
        apply_variable_attributes (var);

        if (var.visibility > variable_visibility::scope)
        {
          diag_record dr (fail (nloc));

          dr << "variable " << var << " has " << var.visibility
             << " visibility but is assigned on a scope";

          if (var.visibility == variable_visibility::target)
            dr << info << "consider changing it to '*: " << var << "'";
        }

        {
          enter_scope sg (d.empty ()
                          ? enter_scope ()
                          : enter_scope (*this, move (d)));
          parse_variable (t, tt, var, tt);
        }

        next_after_newline (t, tt);
        continue;
      }

      // See if this is a directory scope.
      //
      // Note: must be last since we are going to get the next token.
      //
      if (ns.size () == 1 && ns[0].directory () && tt == type::newline)
      {
        token ot (t);

        if (next (t, tt) == type::lcbrace && peek () == type::newline)
        {
          // Make sure not a pattern (see also the target and directory cases
          // above).
          //
          if (ns[0].pattern)
            fail (nloc) << "pattern in " << ns[0];

          next (t, tt); // Newline.
          next (t, tt); // First token inside the block.

          if (at.first)
            fail (at.second) << "attributes before scope directory";
          else
            attributes_pop ();

          // Can contain anything that a top level can.
          //
          {
            enter_scope sg (*this, move (ns[0].dir));
            parse_clause (t, tt);
          }

          if (tt != type::rcbrace)
            fail (t) << "expected name or '}' instead of " << t;

          next (t, tt);                    // Presumably newline after '}'.
          next_after_newline (t, tt, '}'); // Should be on its own line.
          continue;
        }

        t = ot;
        // Fall through to fail.
      }

      fail (t) << "unexpected " << t << " after " << ns;
    }

    return parsed;
  }

  void parser::
  parse_clause_block (token& t, type& tt, bool skip, const string& k)
  {
    next (t, tt); // Get newline.
    next (t, tt); // First token inside the block.

    if (skip)
      skip_block (t, tt);
    else
      parse_clause (t, tt);

    if (tt != type::rcbrace)
      fail (t) << "expected name or '}' instead of " << t
               << " at the end of " << k << "-block";

    next (t, tt);                    // Presumably newline after '}'.
    next_after_newline (t, tt, '}'); // Should be on its own line.
  }

  void parser::
  parse_variable_block (token& t, type& tt,
                        optional<pattern_type> pt, const target_type* ptt,
                        string pat, const location& ploc)
  {
    // Parse a target or prerequisite-specific variable block. If type is not
    // NULL, then this is a target type/pattern-specific block.
    //
    // enter: first token of first line in the block (normal lexer mode)
    // leave: rcbrace or eos
    //
    // This is a more restricted variant of parse_clause() that only allows
    // variable assignments.
    //
    tracer trace ("parser::parse_variable_block", &path_);

    while (tt != type::rcbrace && tt != type::eos)
    {
      attributes_push (t, tt);

      // Variable names should not contain patterns so we preserve them here
      // and diagnose in parse_variable_name().
      //
      location nloc (get_location (t));
      names ns (parse_names (t, tt, pattern_mode::preserve, "variable name"));

      if (tt != type::assign  &&
          tt != type::prepend &&
          tt != type::append)
        fail (t) << "expected variable assignment instead of " << t;

      const variable& var (parse_variable_name (move (ns), nloc));
      apply_variable_attributes (var);

      if (prerequisite_ == nullptr                   &&
          var.visibility > variable_visibility::target)
      {
        fail (t) << "variable " << var << " has " << var.visibility
                 << " visibility but is assigned on a target";
      }

      if (pt)
        parse_type_pattern_variable (t, tt,
                                     *pt, *ptt, pat, ploc, // Note: can't move.
                                     var, tt, get_location (t));
      else
        parse_variable (t, tt, var, tt);

      if (tt != type::newline)
        fail (t) << "expected newline instead of " << t;

      next (t, tt);
    }
  }

  void parser::
  parse_recipe (token& t, type& tt,
                const token& start,
                small_vector<shared_ptr<adhoc_rule>, 1>& recipes,
                const target_type* ttype,
                const string& name)
  {
    // Parse a recipe chain.
    //
    // % [<attrs>] [<buildspec>]
    // [if|if!|switch|recipe ...]
    // {{ [<lang> ...]
    //   ...
    // }}
    // ...
    //
    // enter: start is percent or openining multi-curly-brace
    // leave: token past newline after last closing multi-curly-brace
    //
    // If target_ is not NULL, then add the recipe to its adhoc_recipes.
    // Otherwise, return it in recipes (used for pattern rules).

    if (stage_ == stage::boot)
      fail (t) << "ad hoc recipe specified during bootstrap";

    // If we have a recipe, the target is not implied.
    //
    if (target_ != nullptr)
    {
      // @@ What if some members are added later?
      //
      // @@ Also, what happends if redeclared as real dependency, do we
      //    upgrade the members?
      //
      if (target_->decl != target_decl::real)
      {
        target_->decl = target_decl::real;

        if (group* g = target_->is_a<group> ())
        {
          for (const target& m: g->static_members)
            const_cast<target&> (m).decl = target_decl::real; // During load.
        }
        else
        {
          for (target* m (target_->adhoc_member);
               m != nullptr;
               m = m->adhoc_member)
            m->decl = target_decl::real;
        }

        if (default_target_ == nullptr)
          default_target_ = target_;
      }
    }

    bool first (replay_ != replay::play); // First target.
    bool clean (false);                   // Seen recipe that requires cleanup.

    t = start; tt = t.type;
    for (size_t i (0); tt == type::percent || tt == type::multi_lcbrace; ++i)
    {
      // For missing else/default (see below).
      //
      // Note that it may remain NULL if we have, say, an if-condition that
      // evaluates to false and no else. While it may be tempting to get rid
      // of such "holes", it's not easy due to the replay semantics (see the
      // target_ != nullptr block below). So we expect the caller to be
      // prepared to handle this.
      //
      recipes.push_back (nullptr);

      attributes as;
      buildspec bs;
      location bsloc;

      struct data
      {
        const target_type*                       ttype;
        const string&                            name;
        small_vector<shared_ptr<adhoc_rule>, 1>& recipes;
        bool                                     first;
        bool&                                    clean;
        size_t                                   i;
        attributes&                              as;
        buildspec&                               bs;
        const location&                          bsloc;
        function<void (string&&)>                parse_trailer;
      } d {ttype, name, recipes, first, clean, i, as, bs, bsloc, {}};

      d.parse_trailer = [this, &d] (string&& text)
      {
        if (d.first)
        {
          adhoc_rule& ar (*d.recipes.back ());

          // Translate each buildspec entry into action and add it to the
          // recipe entry.
          //
          const location& l (d.bsloc);

          for (metaopspec& m: d.bs)
          {
            meta_operation_id mi (ctx->meta_operation_table.find (m.name));

            if (mi == 0)
              fail (l) << "unknown meta-operation " << m.name;

            const meta_operation_info* mf (
              root_->root_extra->meta_operations[mi]);

            if (mf == nullptr)
              fail (l) << "project " << *root_ << " does not support meta-"
                       << "operation " << ctx->meta_operation_table[mi].name;

            for (opspec& o: m)
            {
              operation_id oi;
              if (o.name.empty ())
              {
                if (mf->operation_pre == nullptr)
                  oi = update_id;
                else
                  // Calling operation_pre() to translate doesn't feel
                  // appropriate here.
                  //
                  fail (l) << "default operation in recipe action" << endf;
              }
              else
                oi = ctx->operation_table.find (o.name);

              if (oi == 0)
                fail (l) << "unknown operation " << o.name;

              const operation_info* of (root_->root_extra->operations[oi]);

              if (of == nullptr)
                fail (l) << "project " << *root_ << " does not support "
                         << "operation " << ctx->operation_table[oi];

              // Note: for now always inner (see match_rule_impl() for
              // details).
              //
              action a (mi, oi);

              // Check for duplicates (local).
              //
              if (find_if (
                    d.recipes.begin (), d.recipes.end (),
                    [a] (const shared_ptr<adhoc_rule>& r)
                    {
                      auto& as (r->actions);
                      return find (as.begin (), as.end (), a) != as.end ();
                    }) != d.recipes.end ())
              {
                fail (l) << "duplicate " << mf->name << '(' << of->name
                         << ") recipe";
              }

              ar.actions.push_back (a);
            }
          }

          // Set the recipe text.
          //
          if (ar.recipe_text (
                *scope_,
                d.ttype != nullptr ? *d.ttype : target_->type (),
                move (text),
                d.as))
            d.clean = true;

          // Verify we have no unhandled attributes.
          //
          for (attribute& a: d.as)
            fail (d.as.loc) << "unknown recipe attribute " << a << endf;
        }

        // Copy the recipe over to the target verifying there are no
        // duplicates (global).
        //
        if (target_ != nullptr)
        {
          const shared_ptr<adhoc_rule>& r (d.recipes[d.i]);

          for (const shared_ptr<adhoc_rule>& er: target_->adhoc_recipes)
          {
            auto& as (er->actions);

            for (action a: r->actions)
            {
              if (find (as.begin (), as.end (), a) != as.end ())
              {
                const meta_operation_info* mf (
                  root_->root_extra->meta_operations[a.meta_operation ()]);

                const operation_info* of (
                  root_->root_extra->operations[a.operation ()]);

                fail (d.bsloc)
                  << "duplicate " << mf->name << '(' << of->name
                  << ") recipe for target " << *target_;
              }
            }
          }

          target_->adhoc_recipes.push_back (r);

          // Note that "registration" of configure_* and dist_* actions
          // (similar to ad hoc rules) is provided by match_adhoc_recipe().
        }
      };

      // Note that this function must be called at most once per iteration.
      //
      auto parse_block = [this, &d] (token& t, type& tt,
                                     bool skip,
                                     const string& kind)
      {
        token st (t); // Save block start token.

        optional<string> lang;
        location lloc;

        // Use value mode to minimize the number of special characters.
        //
        mode (lexer_mode::value, '@');
        if (next (t, tt) == type::newline)
          ;
        else if (tt == type::word)
        {
          lang = t.value;
          lloc = get_location (t);
          next (t, tt); // Newline after <lang>.
        }
        else
          fail (t) << "expected recipe language instead of " << t;

        if (!skip)
        {
          if (d.first)
          {
            // Note that this is always the location of the opening multi-
            // curly-brace, whether we have the header or not. This is relied
            // upon by the rule implementations (e.g., to calculate the first
            // line of the recipe code).
            //
            location loc (get_location (st));

            // @@ We could add an attribute (name= or recipe_name=) to allow
            //    the user specify a friendly name for diagnostics, similar
            //    to rule_name.

            shared_ptr<adhoc_rule> ar;
            if (!lang || icasecmp (*lang, "buildscript") == 0)
            {
              // Buildscript
              //
              ar.reset (
                new adhoc_buildscript_rule (
                  d.name.empty () ? "<ad hoc buildscript recipe>" : d.name,
                  loc,
                  st.value.size ()));
            }
            else if (icasecmp (*lang, "c++") == 0)
            {
              // C++
              //

              // Parse recipe version and optional fragment separator.
              //
              if (tt == type::newline || tt == type::eos)
                fail (t) << "expected c++ recipe version instead of " << t;

              location nloc (get_location (t));
              names ns (parse_names (t, tt, pattern_mode::ignore));

              uint64_t ver;
              try
              {
                if (ns.empty ())
                  throw invalid_argument ("empty");

                if (ns[0].pair)
                  throw invalid_argument ("pair in value");

                ver = convert<uint64_t> (move (ns[0]));
              }
              catch (const invalid_argument& e)
              {
                fail (nloc) << "invalid c++ recipe version: " << e << endf;
              }

              optional<string> sep;
              if (ns.size () != 1)
              try
              {
                if (ns.size () != 2)
                  throw invalid_argument ("multiple names");

                sep = convert<string> (move (ns[1]));

                if (sep->empty ())
                  throw invalid_argument ("empty");
              }
              catch (const invalid_argument& e)
              {
                fail (nloc) << "invalid c++ recipe fragment separator: " << e
                            << endf;
              }

              ar.reset (
                new adhoc_cxx_rule (
                  d.name.empty () ? "<ad hoc c++ recipe>" : d.name,
                  loc,
                  st.value.size (),
                  ver,
                  move (sep)));
            }
            else
              fail (lloc) << "unknown recipe language '" << *lang << "'";

            assert (d.recipes[d.i] == nullptr);
            d.recipes[d.i] = move (ar);
          }
          else
          {
            skip_line (t, tt);
            assert (d.recipes[d.i] != nullptr);
          }
        }
        else
          skip_line (t, tt);

        mode (lexer_mode::foreign, '\0', st.value.size ());
        next_after_newline (t, tt, st); // Should be on its own line.

        if (tt != type::word)
        {
          diag_record dr;

          dr << fail (t) << "unterminated recipe ";
          if (kind.empty ()) dr << "block"; else dr << kind << "-block";

          dr << info (st) << "recipe ";
          if (kind.empty ()) dr << "block"; else dr << kind << "-block";
          dr << " starts here" << endf;
        }

        if (!skip)
          d.parse_trailer (move (t.value));

        next (t, tt);
        assert (tt == type::multi_rcbrace);

        next (t, tt);                          // Newline.
        next_after_newline (t, tt, token (t)); // Should be on its own line.
      };

      auto parse_recipe_directive = [this, &d] (token& t, type& tt,
                                                const string&)
      {
        // Parse recipe directive:
        //
        // recipe <lang> <file>
        //
        // Note that here <lang> is not optional.
        //
        // @@ We could guess <lang> from the extension.

        // Use value mode to minimize the number of special characters.
        //
        mode (lexer_mode::value, '@');

        // Parse <lang>.
        //
        if (next (t, tt) != type::word)
          fail (t) << "expected recipe language instead of " << t;

        location lloc (get_location (t));
        string lang (t.value);
        next (t, tt);

        // Parse <file> as names to get variable expansion, etc.
        //
        location nloc (get_location (t));
        names ns (parse_names (t, tt, pattern_mode::ignore, "file name"));

        path file;
        try
        {
          file = convert<path> (move (ns));
        }
        catch (const invalid_argument& e)
        {
          fail (nloc) << "invalid recipe file path: " << e;
        }

        string text;
        if (d.first)
        {
          // Source relative to the buildfile rather than src scope. In
          // particular, this make sourcing from exported buildfiles work.
          //
          if (file.relative () && path_->path != nullptr)
          {
            // Note: all sourced/included/imported paths are absolute and
            // normalized.
            //
            file = path_->path->directory () / file;
          }

          file.normalize ();

          try
          {
            ifdstream ifs (file);
            text = ifs.read_text ();
          }
          catch (const io_error& e)
          {
            fail (nloc) << "unable to read recipe file " << file << ": " << e;
          }

          shared_ptr<adhoc_rule> ar;
          {
            // This is expected to be the location of the opening multi-curly
            // with the recipe body starting from the following line. So we
            // need to fudge the line number a bit.
            //
            location loc (file, 0, 1);

            if (icasecmp (lang, "buildscript") == 0)
            {
              // Buildscript
              //
              ar.reset (
                new adhoc_buildscript_rule (
                  d.name.empty () ? "<ad hoc buildscript recipe>" : d.name,
                  loc,
                  2)); // Use `{{` and `}}` for dump.

              // Enter as buildfile-like so that it gets automatically
              // distributed. Note: must be consistent with build/export/
              // handling in process_default_target().
              //
              enter_buildfile<buildscript> (file);
            }
            else if (icasecmp (lang, "c++") == 0)
            {
              // C++
              //
              // We expect to find a C++ comment line with version and
              // optional fragment separator before the first non-comment,
              // non-blank line:
              //
              // // c++ <ver> [<sep>]
              //
              string s;
              location sloc (file, 1, 1);
              {
                // Note: observe blank lines for accurate line count.
                //
                size_t b (0), e (0);
                for (size_t m (0), n (text.size ());
                     next_word (text, n, b, e, m, '\n', '\r'), b != n;
                     sloc.line++)
                {
                  s.assign (text, b, e - b);

                  if (!trim (s).empty ())
                  {
                    if (icasecmp (s, "// c++ ", 7) == 0)
                      break;

                    if (s[0] != '/' || s[1] != '/')
                    {
                      b = e;
                      break;
                    }
                  }
                }

                if (b == e)
                  fail (sloc) << "no '// c++ <version> [<separator>]' line";
              }

              uint64_t ver;
              optional<string> sep;
              {
                size_t b (7), e (7);
                if (next_word (s, b, e, ' ', '\t') == 0)
                  fail (sloc) << "missing c++ recipe version" << endf;

                try
                {
                  ver = convert<uint64_t> (build2::name (string (s, b, e - b)));
                }
                catch (const invalid_argument& e)
                {
                  fail (sloc) << "invalid c++ recipe version: " << e << endf;
                }

                if (next_word (s, b, e, ' ', '\t') != 0)
                {
                  sep = string (s, b, e - b);

                  if (next_word (s, b, e, ' ', '\t') != 0)
                    fail (sloc) << "junk after fragment separator";
                }
              }

              ar.reset (
                new adhoc_cxx_rule (
                  d.name.empty () ? "<ad hoc c++ recipe>" : d.name,
                  loc,
                  2,  // Use `{{` and `}}` for dump.
                  ver,
                  move (sep)));

              // Enter as buildfile-like so that it gets automatically
              // distributed. Note: must be consistent with build/export/
              // handling in process_default_target().
              //
              // While ideally we would want to use the cxx{} target type,
              // it's defined in a seperate build system module (which may not
              // even be loaded by this project, so even runtime lookup won't
              // work). So we use file{} instead.
              //
              enter_buildfile<build2::file> (file);
            }
            else
              fail (lloc) << "unknown recipe language '" << lang << "'";
          }

          assert (d.recipes[d.i] == nullptr);
          d.recipes[d.i] = move (ar);
        }
        else
          assert (d.recipes[d.i] != nullptr);

        d.parse_trailer (move (text));

        next_after_newline (t, tt);
      };

      bsloc = get_location (t); // Fallback location.

      if (tt == type::percent)
      {
        // Similar code to parse_buildspec() except here we recognize
        // attributes and newlines.
        //
        mode (lexer_mode::buildspec, '@', 1 /* recognize newline */);

        next_with_attributes (t, tt);
        attributes_push (t, tt, true /* standalone */);

        // Handle recipe attributes. We divide them into common and recipe
        // language-specific.
        //
        // TODO: handle and erase common attributes if/when we have any.
        //
        as = attributes_pop ();

        // Handle the buildspec.
        //
        // @@ TODO: diagnostics is a bit off ("operation or target").
        //
        if (tt != type::newline && tt != type::eos)
        {
          const location& l (bsloc = get_location (t));
          bs = parse_buildspec_clause (t, tt);

          // Verify we have no targets and assign default meta-operations.
          //
          // Note that here we don't bother with lifting operations to meta-
          // operations like we do in the driver (this seems unlikely to be a
          // pain point).
          //
          for (metaopspec& m: bs)
          {
            for (opspec& o: m)
            {
              if (!o.empty ())
                fail (l) << "target in recipe action";
            }

            if (m.name.empty ())
              m.name = "perform";
          }
        }
        else
        {
          // Default is perform(update).
          //
          bs.push_back (metaopspec ("perform"));
          bs.back ().push_back (opspec ("update"));
        }

        expire_mode ();
        next_after_newline (t, tt, "recipe action");

        // See if this is if-else/switch or `recipe`.
        //
        // We want the keyword test similar to parse_clause() but we cannot do
        // it if replaying. So we skip it with understanding that if it's not
        // a keywords, then it would have been an error while saving and we
        // would have never actual gotten to replay in this case.
        //
        if (tt == type::word && (!first || keyword (t)))
        {
          const string& n (t.value);

          // Note that we may have if without else and switch without default.
          // We treat such cases as if no recipe was specified (this can be
          // handy if we want to provide a custom recipe but only on certain
          // platforms or some such).

          if (n == "if" || n == "if!")
          {
            parse_if_else (t, tt, true /* multi */,
                           parse_block, parse_recipe_directive);
            continue;
          }
          else if (n == "switch")
          {
            parse_switch (t, tt, true /* multi */,
                          parse_block, parse_recipe_directive);
            continue;
          }
          else if (n == "recipe")
          {
            parse_recipe_directive (t, tt, "" /* kind */);
            continue;
          }

          // Fall through.
        }

        if (tt != type::multi_lcbrace)
          fail (t) << "expected recipe block or 'recipe' instead of " << t;

        // Fall through.
      }
      else
      {
        // Default is perform(update).
        //
        bs.push_back (metaopspec ("perform"));
        bs.back ().push_back (opspec ("update"));
      }

      parse_block (t, tt, false /* skip */, "" /* kind */);
    }

    // If we have a recipe that needs cleanup, register an operation callback
    // for this project unless it has already been done.
    //
    if (clean)
    {
      action a (perform_clean_id);
      auto f (&adhoc_rule::clean_recipes_build);

      // First check if we have already done this.
      //
      auto p (root_->operation_callbacks.equal_range (a));
      for (; p.first != p.second; ++p.first)
      {
        auto t (
          p.first->second.pre.target<scope::operation_callback::callback*> ());

        if (t != nullptr && *t == f)
          break;
      }

      // It feels natural to clean up recipe builds as a post operation but
      // that prevents the (otherwise-empty) out root directory to be cleaned
      // up (via the standard fsdir{} chain).
      //
      if (p.first == p.second)
        root_->operation_callbacks.emplace (
          a, scope::operation_callback {f, nullptr /*post*/});
    }
  }

  vector<reference_wrapper<target>> parser::
  enter_explicit_members (group_names_loc&& gns, bool implied)
  {
    tracer trace ("parser::enter_explicit_members", &path_);

    names& ns (gns.ns);
    const location& loc (gns.member_loc);

    vector<reference_wrapper<target>> r;
    r.reserve (ns.size ());

    group& g (target_->as<group> ());
    auto& ms (g.static_members);

    for (size_t i (0); i != ns.size (); ++i)
    {
      name&& n (move (ns[i]));
      name&& o (n.pair ? move (ns[++i]) : name ());

      if (n.qualified ())
        fail (loc) << "project name in target " << n;

      // We derive the path unless the target name ends with the '...' escape
      // which here we treat as the "let the rule derive the path" indicator
      // (see target::split_name() for details). This will only be useful for
      // referring to group members that are managed by the group's matching
      // rule. Note also that omitting '...' for such a member could be used
      // to override the file name, provided the rule checks if the path has
      // already been derived before doing it itself.
      //
      // @@ What can the ad hoc recipe/rule do differently here? Maybe get
      //    path from dynamic targets? Maybe we will have custom path
      //    derivation support in buildscript in the future?
      //
      bool escaped;
      {
        const string& v (n.value);
        size_t p (v.size ());

        escaped = (p > 3 &&
                   v[--p] == '.' && v[--p] == '.' && v[--p] == '.' &&
                   v[--p] != '.');
      }

      target& m (enter_target::insert_target (*this,
                                              move (n), move (o),
                                              implied,
                                              loc, trace));

      if (g == m)
        fail (loc) << "explicit group member " << m << " is group itself";

      // Add as static member skipping duplicates.
      //
      if (find (ms.begin (), ms.end (), m) == ms.end ())
      {
        if (m.group == nullptr)
          m.group = &g;
        else if (m.group != &g)
          fail (loc) << g << " group member " << m << " already belongs to "
                     << "group " << *m.group;

        ms.push_back (m);
      }

      if (!escaped)
      {
        if (file* ft = m.is_a<file> ())
          ft->derive_path ();
      }

      r.push_back (m);
    }

    return r;
  }

  vector<reference_wrapper<target>> parser::
  enter_adhoc_members (group_names_loc&& gns, bool implied)
  {
    tracer trace ("parser::enter_adhoc_members", &path_);

    names& ns (gns.ns);
    const location& loc (gns.member_loc);

    if (target_->is_a<group> ())
      fail (loc) << "ad hoc group primary member " << *target_
                 << " is explicit group";

    vector<reference_wrapper<target>> r;
    r.reserve (ns.size ());

    for (size_t i (0); i != ns.size (); ++i)
    {
      name&& n (move (ns[i]));
      name&& o (n.pair ? move (ns[++i]) : name ());

      if (n.qualified ())
        fail (loc) << "project name in target " << n;

      // We derive the path unless the target name ends with the '...' escape
      // which here we treat as the "let the rule derive the path" indicator
      // (see target::split_name() for details). This will only be useful for
      // referring to ad hoc members that are managed by the group's matching
      // rule. Note also that omitting '...' for such a member could be used
      // to override the file name, provided the rule checks if the path has
      // already been derived before doing it itself.
      //
      bool escaped;
      {
        const string& v (n.value);
        size_t p (v.size ());

        escaped = (p > 3 &&
                   v[--p] == '.' && v[--p] == '.' && v[--p] == '.' &&
                   v[--p] != '.');
      }

      target& m (enter_target::insert_target (*this,
                                              move (n), move (o),
                                              implied,
                                              loc, trace));

      if (target_ == &m)
        fail (loc) << "ad hoc group member " << m << " is primary target";

      if (m.is_a<group> ())
        fail (loc) << "ad hoc group member " << m << " is explicit group";

      // Add as an ad hoc member at the end of the chain skipping duplicates.
      //
      {
        const_ptr<target>* mp (&target_->adhoc_member);
        for (; *mp != nullptr; mp = &(*mp)->adhoc_member)
        {
          if (*mp == &m)
          {
            mp = nullptr;
            break;
          }
        }

        if (mp != nullptr)
        {
          if (m.group == nullptr)
            m.group = target_;
          else if (m.group != target_)
            fail (loc) << *target_ << " ad hoc group member " << m
                       << " already belongs to group " << *m.group;
          *mp = &m;
        }
      }

      if (!escaped)
      {
        if (file* ft = m.is_a<file> ())
          ft->derive_path ();
      }

      r.push_back (m);
    }

    return r;
  }

  small_vector<pair<reference_wrapper<target>,
                    vector<reference_wrapper<target>>>, 1> parser::
  enter_targets (names&& tns, const location& tloc, // Target names.
                 group_names&& gns,                 // Group member names.
                 size_t prereq_size,
                 const attributes& tas)             // Target attributes.
  {
    // Enter all the targets (normally we will have just one) and their group
    // members.
    //
    tracer trace ("parser::enter_targets", &path_);

    small_vector<pair<reference_wrapper<target>,
                      vector<reference_wrapper<target>>>, 1> tgs;

    for (size_t i (0); i != tns.size (); ++i)
    {
      name&& n (move (tns[i]));
      name&& o (n.pair ? move (tns[++i]) : name ());

      if (n.qualified ())
        fail (tloc) << "project name in target " << n;

      // Make sure none of our targets are patterns.
      //
      if (n.pattern)
        fail (tloc) << "unexpected pattern in target " << n <<
          info << "ad hoc pattern rule may not be combined with other "
               << "targets or patterns";

      enter_target tg (*this,
                       move (n), move (o),
                       false /* implied */,
                       tloc, trace);

      if (!tas.empty ())
        apply_target_attributes (*target_, tas);

      // Enter group members.
      //
      vector<reference_wrapper<target>> gms;
      if (!gns.empty ())
      {
        // Note: index after the pair increment.
        //
        group_names_loc& g (gns[i]);

        if (g.expl && !target_->is_a<group> ())
          fail (g.group_loc) << *target_ << " is not group target";

        gms = g.expl
          ? enter_explicit_members (move (g), false /* implied */)
          : enter_adhoc_members (move (g), false /* implied */);
      }

      if (default_target_ == nullptr)
        default_target_ = target_;

      target_->prerequisites_state_.store (2, memory_order_relaxed);
      target_->prerequisites_.reserve (prereq_size);
      tgs.emplace_back (*target_, move (gms));
    }

    return tgs;
  }

  void parser::
  apply_target_attributes (target& t, const attributes& as)
  {
    const location& l (as.loc);

    for (auto& a: as)
    {
      const string& n (a.name);
      const value& v (a.value);

      // rule_hint=
      // liba@rule_hint=
      //
      size_t p (string::npos);
      if (n == "rule_hint" ||
          ((p = n.find ('@')) != string::npos &&
           n.compare (p + 1, string::npos, "rule_hint") == 0))
      {
        // Resolve target type, if specified.
        //
        const target_type* tt (nullptr);
        if (p != string::npos)
        {
          string t (n, 0, p);
          tt = scope_->find_target_type (t);

          if (tt == nullptr)
            fail (l) << "unknown target type " << t << " in rule_hint "
                     << "attribute";
        }

        // The rule hint value is vector<pair<optional<string>, string>> where
        // the first half is the operation and the second half is the hint.
        // Absent operation is used as a fallback for update/clean.
        //
        const names& ns (v.as<names> ());

        for (auto i (ns.begin ()); i != ns.end (); ++i)
        {
          operation_id oi (default_id);
          if (i->pair)
          {
            const name& n (*i++);

            if (!n.simple ())
              fail (l) << "expected operation name instead of " << n
                       << " in rule_hint attribute";

            const string& v (n.value);

            if (!v.empty ())
            {
              oi = ctx->operation_table.find (v);

              if (oi == 0)
                fail (l) << "unknown operation " << v << " in rule_hint "
                         << "attribute";

              if (root_->root_extra->operations[oi] == nullptr)
                fail (l) << "project " << *root_ << " does not support "
                         << "operation " << ctx->operation_table[oi]
                         << " specified in rule_hint attribute";
            }
          }

          const name& n (*i);

          if (!n.simple () || n.empty ())
            fail (l) << "expected hint instead of " << n << " in rule_hint "
                     << "attribute";

          t.rule_hints.insert (tt, oi, n.value);
        }
      }
      else
        fail (l) << "unknown target attribute " << a;
    }
  }

  void parser::
  parse_dependency (token& t, token_type& tt,
                    names&& tns, const location& tloc, // Target names.
                    group_names&& gns,                 // Group member names.
                    names&& pns, const location& ploc, // Prereq names.
                    const attributes& tas)             // Target attributes.
  {
    // Parse a dependency chain and/or a target/prerequisite-specific variable
    // assignment/block and/or recipe block(s).
    //
    // enter: colon or newline (latter only in recursive calls)
    // leave: - first token on the next line
    //
    tracer trace ("parser::parse_dependency", &path_);

    // Diagnose conditional prerequisites. Note that we want to diagnose this
    // even if pns is empty (think empty variable expansion; the literal "no
    // prerequisites" case is handled elsewhere).
    //
    // @@ TMP For now we only do it during the dist meta-operation. In the
    //        future we should tighten this to any meta-operation provided
    //        the dist module is loaded.
    //
    // @@ TMP For now it's a warning because we have dependencies like
    //        cli.cxx{foo}: cli{foo} which are not currently possible to
    //        rewrite (cli.cxx{} is not always registered).
    //
    if (condition_ &&
        ctx->current_mif != nullptr &&
        ctx->current_mif->id == dist_id)
    {
      // Only issue the warning for the projects being distributed. In
      // particular, this makes sure we don't complain about imported
      // projects. Note: use amalgamation to cover bundled subprojects.
      //
      auto* dm (root_->bundle_scope ()->find_module<dist::module> (
                  dist::module::name));

      if (dm != nullptr && dm->distributed)
      {
        warn (tloc) << "conditional dependency declaration may result in "
                    << "incomplete distribution" <<
          info (ploc) << "prerequisite declared here" <<
          info (*condition_) << "conditional buildfile fragment starts here" <<
          info << "instead use 'include' prerequisite-specific variable to "
               << "conditionally include prerequisites" <<
          info << "for example: "
               << "<target>: <prerequisite>: include = (<condition>)" <<
          info << "for details, see https://github.com/build2/HOWTO/blob/"
               << "master/entries/keep-build-graph-config-independent.md";
      }
    }

    // First enter all the targets.
    //
    small_vector<pair<reference_wrapper<target>,
                      vector<reference_wrapper<target>>>, 1>
      tgs (enter_targets (move (tns), tloc, move (gns), pns.size (), tas));

    // Now enter each prerequisite into each target.
    //
    for (auto i (pns.begin ()); i != pns.end (); ++i)
    {
      // We cannot reuse the names if we (potentially) may need to pass them
      // as targets in case of a chain (see below).
      //
      name n (tt != type::colon ? move (*i) : *i);

      // See also scope::find_prerequisite_key().
      //
      auto rp (scope_->find_target_type (n, ploc));
      const target_type* t (rp.first);
      optional<string>& e (rp.second);

      if (t == nullptr)
      {
        if (n.proj)
        {
          // If the target type is unknown then no phase 2 import (like
          // rule-specific search) can possibly succeed so we can fail now and
          // with a more accurate reason. See import2(names) for background.
          //
          diag_record dr;
          dr << fail (ploc) << "unable to import target " << n;
          import_suggest (dr, *n.proj, nullptr, string (), false);
        }
        else
        {
          fail (ploc) << "unknown target type " << n.type <<
            info << "perhaps the module that defines this target type is "
                 << "not loaded by project " << *scope_->root_scope ();
        }
      }

      if (t->factory == nullptr)
        fail (ploc) << "abstract target type " << t->name << "{}";

      // Current dir collapses to an empty one.
      //
      if (!n.dir.empty ())
        n.dir.normalize (false /* actual */, true);

      // @@ OUT: for now we assume the prerequisite's out is undetermined. The
      // only way to specify an src prerequisite will be with the explicit
      // @-syntax.
      //
      // Perhaps use @file{foo} as a way to specify it is in the out tree,
      // e.g., to suppress any src searches? The issue is what to use for such
      // a special indicator. Also, one can easily and natually suppress any
      // searches by specifying the absolute path.
      //
      name o;
      if (n.pair)
      {
        assert (n.pair == '@');

        ++i;
        o = tt != type::colon ? move (*i) : *i;

        if (!o.directory ())
          fail (ploc) << "expected directory after '@'";

        o.dir.normalize (); // Note: don't collapse current to empty.

        // Make sure out and src are parallel unless both were specified as
        // absolute. We make an exception for this case because out may be
        // used to "tag" imported targets (see cc::search_library()). So it's
        // sort of the "I know what I am doing" escape hatch (it would have
        // been even better to verify such a target is outside any project
        // but that won't be cheap).
        //
        // For now we require that both are either relative or absolute.
        //
        // See similar code for targets in scope::find_target_type().
        //
        if (n.dir.absolute () && o.dir.absolute ())
          ;
        else if (n.dir.empty () && o.dir.current ())
          ;
        else if (o.dir.relative () &&
                 n.dir.relative () &&
                 o.dir == n.dir)
          ;
        else
          fail (ploc) << "prerequisite output directory " << o.dir
                      << " must be parallel to source directory " << n.dir;
      }

      prerequisite p (move (n.proj),
                      *t,
                      move (n.dir),
                      move (o.dir),
                      move (n.value),
                      move (e),
                      *scope_);

      for (auto i (tgs.begin ()), e (tgs.end ()); i != e; )
      {
        // Move last prerequisite (which will normally be the only one).
        //
        target& t (i->first);
        t.prerequisites_.push_back (++i == e
                                    ? move (p)
                                    : prerequisite (p, memory_order_relaxed));
      }
    }

    // Call the specified parsing function (either variable or block) for each
    // target in tgs (for_each_t) or for the last pns.size() prerequisites of
    // each target (for_each_p).
    //
    // We handle multiple targets and/or prerequisites by replaying the tokens
    // (see the target-specific case comments for details). The function
    // signature for for_each_t (see for_each on the gm argument semantics):
    //
    // void (token& t, type& tt, optional<bool> gm)
    //
    // And for for_each_p:
    //
    // void (token& t, type& tt)
    //
    auto for_each_t = [this, &t, &tt, &tgs] (auto&& f)
    {
      // We need replay if we have multiple targets or group members.
      //
      replay_guard rg (*this, tgs.size () > 1 || !tgs[0].second.empty ());

      for (auto ti (tgs.begin ()), te (tgs.end ()); ti != te; )
      {
        target& tg (ti->first);
        const vector<reference_wrapper<target>>& gms (ti->second);

        {
          enter_target g (*this, tg);
          f (t, tt, nullopt);
        }

        if (!gms.empty ())
        {
          bool expl (tg.is_a<group> ());

          for (target& gm: gms)
          {
            rg.play (); // Replay.

            enter_target g (*this, gm);
            f (t, tt, expl);
          }
        }

        if (++ti != te)
          rg.play (); // Replay.
      }
    };

    auto for_each_p = [this, &t, &tt, &tgs, &pns] (auto&& f)
    {
      replay_guard rg (*this, tgs.size () > 1 || pns.size () > 1);

      for (auto ti (tgs.begin ()), te (tgs.end ()); ti != te; )
      {
        target& tg (ti->first);
        enter_target g (*this, tg);

        for (size_t pn (tg.prerequisites_.size ()), pi (pn - pns.size ());
             pi != pn; )
        {
          enter_prerequisite pg (*this, tg.prerequisites_[pi]);

          f (t, tt);

          if (++pi != pn)
            rg.play (); // Replay.
        }

        if (++ti != te)
          rg.play (); // Replay.
      }
    };

    // Do we have a dependency chain and/or prerequisite-specific variable
    // assignment/block? If not, check for the target-specific variable block
    // and/or recipe block(s).
    //
    if (tt != type::colon)
    {
      next_after_newline (t, tt); // Must be a newline then.

      // Note: watch out for non-block cases like this:
      //
      // foo: bar
      // {hxx ixx}: install = true
      //
      if (tt == type::percent       ||
          tt == type::multi_lcbrace ||
          (tt == type::lcbrace && peek () == type::newline))
      {
        // Parse the block(s) for each target.
        //
        // Note: similar code to the version in parse_clause().
        //
        auto parse = [
          this,
          st = token (t), // Save start token (will be gone on replay).
          recipes = small_vector<shared_ptr<adhoc_rule>, 1> ()]
          (token& t, type& tt, optional<bool> gm) mutable
        {
          token rt; // Recipe start token.

          // The variable block, if any, should be first.
          //
          if (st.type == type::lcbrace)
          {
            next (t, tt); // Newline.
            next (t, tt); // First token inside the variable block.

            // Skip explicit group members (see the block case above for
            // background).
            //
            if (!gm || !*gm)
              parse_variable_block (t, tt);
            else
              skip_block (t, tt);

            if (tt != type::rcbrace)
              fail (t) << "expected '}' instead of " << t;

            next (t, tt);                    // Newline.
            next_after_newline (t, tt, '}'); // Should be on its own line.

            if (tt != type::percent && tt != type::multi_lcbrace)
              return;

            rt = t;
          }
          else
            rt = st;

          // If this is a group member then we know we are replaying and can
          // skip the recipe.
          //
          if (gm)
          {
            replay_skip ();
            next (t, tt);
            return;
          }

          parse_recipe (t, tt, rt, recipes);
        };

        for_each_t (parse);
      }

      return;
    }

    // If we are here, then this can be one of three things:
    //
    // 1. A prerequisite-specific variable bloc:
    //
    //    foo: bar:
    //    {
    //      x = y
    //    }
    //
    // 2. A prerequisite-specific variable asignment:
    //
    //    foo: bar: x = y
    //
    // 3. A further dependency chain:
    //
    //    foo: bar: baz ...
    //
    // What should we do if there are no prerequisites, for example, because
    // of an empty wildcard result or empty variable expansion? We can fail or
    // we can ignore. In most cases, however, this is probably an error (for
    // example, forgetting to checkout a git submodule) so let's not confuse
    // the user and fail (one can always handle the optional prerequisites
    // case with a variable and an if).
    //
    // On the other hand, we allow just empty prerequisites (which is also the
    // more common case by far) and so it's strange that we don't allow the
    // same with, say, `include = false`:
    //
    // exe{foo}: cxx{$empty}                  # Ok.
    // exe{foo}: cxx{$empty}: include = false # Not Ok?
    //
    // So let's ignore in the first two cases (variable block and assignment)
    // for consistency. The dependency chain is iffy both conceptually and
    // implementation-wise (it could be followed by a variable block). So
    // let's keep it an error for now.
    //
    // Note that the syntactically-empty prerequisite list is still an error:
    //
    // exe{foo}: : include = false # Error.
    //
    next_with_attributes (t, tt); // Recognize attributes after `:`.

    auto at (attributes_push (t, tt));

    if (tt == type::newline || tt == type::eos)
    {
      attributes_pop (); // Must be none since can't be standalone.

      // There must be a block.
      //
      if (next_after_newline (t, tt) != type::lcbrace)
        fail (t) << "expected '{' instead of " << t;

      if (next (t, tt) != type::newline)
        fail (t) << "expected newline after '{'";

      // Parse the block for each prerequisites of each target.
      //
      if (!pns.empty ())
        for_each_p ([this] (token& t, token_type& tt)
                    {
                      next (t, tt); // First token inside the block.

                      parse_variable_block (t, tt);

                      if (tt != type::rcbrace)
                        fail (t) << "expected '}' instead of " << t;
                    });
      else
      {
        skip_block (t, tt);
        if (tt != type::rcbrace)
          fail (t) << "expected '}' instead of " << t;
      }

      next (t, tt);                    // Presumably newline after '}'.
      next_after_newline (t, tt, '}'); // Should be on its own line.
    }
    else
    {
      // @@ PAT: currently we pattern-expand prerequisite-specific vars.
      //
      const location loc (get_location (t));
      names ns (parse_names (t, tt, pattern_mode::expand));

      // Prerequisite-specific variable assignment.
      //
      if (tt == type::assign || tt == type::prepend || tt == type::append)
      {
        type at (tt);

        const variable& var (parse_variable_name (move (ns), loc));
        apply_variable_attributes (var);

        // Parse the assignment for each prerequisites of each target.
        //
        if (!pns.empty ())
          for_each_p ([this, &var, at] (token& t, token_type& tt)
                      {
                        parse_variable (t, tt, var, at);
                      });
        else
          skip_line (t, tt);

        next_after_newline (t, tt);

        // Check we don't also have a variable block:
        //
        // foo: bar: x = y
        // {
        //   ...
        // }
        //
        if (tt == type::lcbrace && peek () == type::newline)
          fail (t) << "variable assignment block after variable assignment";
      }
      //
      // Dependency chain.
      //
      else
      {
        if (pns.empty ())
          fail (ploc) << "no prerequisites in dependency chain";

        // @@ This is actually ambiguous: prerequisite or target attributes
        //    (or both or neither)? Perhaps this should be prerequisites for
        //    the same reason as below (these are prerequsites first).
        //
        if (at.first)
          fail (at.second) << "attributes before prerequisites";
        else
          attributes_pop ();

        // Note that we could have "pre-resolved" these prerequisites to
        // actual targets or, at least, made their directories absolute. We
        // don't do it for ease of documentation: with the current semantics
        // we just say that the dependency chain is equivalent to specifying
        // each dependency separately.
        //
        // Also note that supporting target group specification in chains will
        // be complicated. For example, what if prerequisites that have group
        // members don't end up being chained? Do we just silently drop them?
        // Also, these are prerequsites first that happened to be reused as
        // target names so perhaps it is the right thing not to support,
        // conceptually.
        //
        parse_dependency (t, tt,
                          move (pns), ploc,
                          {} /* group names */,
                          move (ns), loc,
                          attributes () /* target attributes */);
      }
    }
  }

  void parser::
  source_buildfile (istream& is,
                    const path_name& in,
                    const location& loc,
                    optional<bool> deft)
  {
    tracer trace ("parser::source_buildfile", &path_);

    l5 ([&]{trace (loc) << "entering " << in;});

    const buildfile* bf (in.path != nullptr
                         ? &enter_buildfile<buildfile> (*in.path)
                         : nullptr);

    const path_name* op (path_);
    path_ = &in;

    lexer l (is, *path_);
    lexer* ol (lexer_);
    lexer_ = &l;

    target* odt;
    if (!deft || *deft)
      odt = default_target_;

    if (deft && *deft)
      default_target_ = nullptr;

    token t;
    type tt;
    next (t, tt);
    parse_clause (t, tt);

    if (tt != type::eos)
      fail (t) << "unexpected " << t;

    if (deft && *deft)
    {
      if (stage_ != stage::boot && stage_ != stage::root)
        process_default_target (t, bf);
    }

    if (!deft || *deft)
      default_target_ = odt;

    lexer_ = ol;
    path_ = op;

    l5 ([&]{trace (loc) << "leaving " << in;});
  }

  void parser::
  parse_source (token& t, type& tt)
  {
    // source [<attrs>] <path>+
    //

    // The rest should be a list of buildfiles. Parse them as names in the
    // value mode to get variable expansion and directory prefixes. Also
    // handle optional attributes.
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);
    attributes_push (t, tt);

    bool nodt (false); // Source buildfile without default target semantics.
    {
      attributes as (attributes_pop ());
      const location& l (as.loc);

      for (const attribute& a: as)
      {
        const string& n (a.name);

        if (n == "no_default_target")
        {
          nodt = true;
        }
        else
          fail (l) << "unknown source directive attribute " << a;
      }
    }

    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::expand, "path", nullptr)
              : names ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.typed () || n.value.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path.
      //
      path p (move (n.dir));
      p /= path (move (n.value));

      // If the path is relative then use the src directory corresponding
      // to the current directory scope.
      //
      if (scope_->src_path_ != nullptr && p.relative ())
        p = scope_->src_path () / p;

      p.normalize ();

      try
      {
        ifdstream ifs (p);
        source_buildfile (ifs,
                          path_name (p),
                          get_location (t),
                          nodt ? optional<bool> {} : false);
      }
      catch (const io_error& e)
      {
        fail (l) << "unable to read buildfile " << p << ": " << e;
      }
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_include (token& t, type& tt)
  {
    // include <path>+
    //

    tracer trace ("parser::parse_include", &path_);

    if (stage_ == stage::boot)
      fail (t) << "inclusion during bootstrap";

    // The rest should be a list of buildfiles. Parse them as names in the
    // value mode to get variable expansion and directory prefixes.
    //
    mode (lexer_mode::value, '@');
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::expand, "path", nullptr)
              : names ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.typed () || n.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path. If it is a directory, then append
      // 'buildfile'.
      //
      path p (move (n.dir));

      bool a;
      if (n.value.empty ())
        a = true;
      else
      {
        a = path::traits_type::is_separator (n.value.back ());

        try
        {
          p /= path (move (n.value));
        }
        catch (const invalid_path& e)
        {
          fail (l) << "invalid include path '" << e.path << "'";
        }
      }

      if (a)
      {
        // This shouldn't happen but let's make sure.
        //
        if (root_->root_extra == nullptr)
          fail (l) << "buildfile naming scheme is not yet known";

        p /= root_->root_extra->buildfile_file;
      }

      l6 ([&]{trace (l) << "relative path " << p;});

      // Determine new out_base.
      //
      dir_path out_base;

      try
      {
        if (p.relative ())
        {
          out_base = scope_->out_path () / p.directory ();
          out_base.normalize ();
        }
        else
        {
          p.normalize ();

          // Make sure the path is in this project. Include is only meant
          // to be used for intra-project inclusion (plus amalgamation).
          //
          bool in_out (false);
          if (!p.sub (root_->src_path ()) &&
              !(in_out = p.sub (root_->out_path ())))
            fail (l) << "out of project include " << p;

          out_base = in_out
            ? p.directory ()
            : out_src (p.directory (), *root_);
        }
      }
      catch (const invalid_path&)
      {
        // The failure reason can only be the specified 'go past the root'
        // path. Let's print the original path.
        //
        fail (l) << "invalid include path '" << (a ? p.directory () : p)
                 << "'";
      }

      // Switch the scope. Note that we need to do this before figuring
      // out the absolute buildfile path since we may switch the project
      // root and src_root with it (i.e., include into a sub-project).
      //
      enter_scope sg (*this, out_base, true /* absolute & normalized */);

      if (root_ == nullptr)
        fail (l) << "out of project include from " << out_base;

      // Use the new scope's src_base to get absolute buildfile path if it is
      // relative.
      //
      if (p.relative ())
        p = scope_->src_path () / p.leaf ();

      l6 ([&]{trace (l) << "absolute path " << p;});

      // Note: may be "new" root.
      //
      if (!root_->root_extra->insert_buildfile (p))
      {
        l5 ([&]{trace (l) << "skipping already included " << p;});
        continue;
      }

      // Note: see a variant of this in parse_import().
      //
      // Clear/restore if/switch location.
      //
      // We do it here but not in parse_source since the included buildfile is
      // in a sense expected to be a standalone entity (think a file included
      // from an export stub).
      //
      auto g = make_guard ([this, old = condition_] () mutable
                           {
                             condition_ = old;
                           });
      condition_ = nullopt;

      try
      {
        ifdstream ifs (p);
        source_buildfile (ifs,
                          path_name (p),
                          get_location (t),
                          true /* default_target */);
      }
      catch (const io_error& e)
      {
        fail (l) << "unable to read buildfile " << p << ": " << e;
      }
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_run (token& t, type& tt)
  {
    // run <name> [<arg>...]
    //
    // Note that if the result of executing the program can be affected by
    // environment variables and this result can in turn affect the build
    // result, then such variables should be reported with the
    // config.environment directive.

    // Parse the command line as names in the value mode to get variable
    // expansion, etc.
    //
    mode (lexer_mode::value);
    next (t, tt);
    const location l (get_location (t));

    strings args;
    try
    {
      args = convert<strings> (
        tt != type::newline && tt != type::eos
        ? parse_names (t, tt, pattern_mode::expand, "argument", nullptr)
        : names ());
    }
    catch (const invalid_argument& e)
    {
      fail (l) << "invalid run argument: " << e.what ();
    }

    if (args.empty () || args[0].empty ())
      fail (l) << "expected executable name after run";

    cstrings cargs;
    cargs.reserve (args.size () + 1);
    transform (args.begin (),
               args.end (),
               back_inserter (cargs),
               [] (const string& s) {return s.c_str ();});
    cargs.push_back (nullptr);

    // Note: we are in the serial load phase and so no diagnostics buffering
    // is needed.
    //
    process pr (run_start (3            /* verbosity */,
                           cargs,
                           0            /* stdin  */,
                           -1           /* stdout */,
                           2            /* stderr */,
                           nullptr      /* env    */,
                           dir_path ()  /* cwd    */,
                           l));
    try
    {
      // While a failing process could write garbage to stdout, for simplicity
      // let's assume it is well behaved.
      //
      ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

      // If there is an error in the output, our diagnostics will look like
      // this:
      //
      // <stdout>:2:3 error: unterminated single quote
      //   buildfile:3:4 info: while parsing foo output
      //
      {
        auto df = make_diag_frame (
          [this, &args, &l](const diag_record& dr)
          {
            dr << info (l) << "while parsing " << args[0] << " output";
          });

        source_buildfile (is,
                          path_name ("<stdout>"),
                          l,
                          false /* default_target */);
      }

      is.close (); // Detect errors.
    }
    catch (const io_error& e)
    {
      if (run_wait (cargs, pr, l))
        fail (l) << "io error reading " << cargs[0] << " output: " << e;

      // If the child process has failed then assume the io error was
      // caused by that and let run_finish() deal with it.
    }

    run_finish (cargs, pr, 2 /* verbosity */, false /* omit_normal */, l);

    next_after_newline (t, tt);
  }

  void parser::
  parse_config (token& t, type& tt)
  {
    tracer trace ("parser::parse_config", &path_);

    // General config format:
    //
    // config [<var-attrs>] <var>[?=[<val-attrs>]<default-val>]
    //

    // Make sure only appears in root.build.
    //
    if (stage_ != stage::root)
      fail (t) << "configuration variable outside of project's "
               << root_->root_extra->root_file;

    // Enforce the config.<project> prefix.
    //
    // Note that this could be a subproject and it could be unnamed (e.g., the
    // tests subproject). The current thinking is to use hierarchical names
    // like config.<project>.tests.remote for subprojects, similar to how we
    // do the same for submodules (e.g., cxx.config). Of course, the
    // subproject could also be some named third-party top-level project that
    // we just happened to amalgamate. So what we are going to do is enforce
    // the config[.**].<project>.** pattern where <project> is the innermost
    // named project.
    //
    // Note that we also allow just the config.<project> name which can be
    // used by tools (such as source code generators) that use themselves in
    // their own build. This is a bit of an advanced/experimental setup so
    // we leave this undocumented for now.
    //
    // What should we do if there is no named project? We used to fail but
    // there are valid cases where this can happen, for example, a standalone
    // build of an unnamed tests subproject in order to test an installed
    // library. Doing anything fuzzy like requiring at least a four-component
    // name in this case is probably not worth the trouble: it's possible the
    // subproject needs some configuration values from it amalgamation (in
    // which case it will be duplicating them in its root.build file). So
    // for now we allow this trusting the user knows what they are doing.
    //
    // There is another special case: a buildfile imported from another
    // project. In this case we also allow <project> to be the imported
    // project name in addition to importing. The thinking here is that an
    // imported buildfile is in a sense like a module (may provide rules which
    // may require configuration, etc) and should be able to use its own
    // project name (which is often the corresponding tool name) in the
    // configuration variables, just like modules. In this case we use the
    // imported project name as the reporting module name (but which can
    // be overridden with config.report.module attribute).
    //
    const location loc (get_location (t));

    // We are now in the normal lexing mode and we let the lexer handle `?=`.
    //
    next_with_attributes (t, tt);

    // Get variable attributes, if any, and deal with the special config.*
    // attributes as well as null. Since currently they can only appear in the
    // config directive, we handle them in an ad hoc manner.
    //
    attributes_push (t, tt);
    attributes& as (attributes_top ());

    bool nullable (false);
    optional<string> report;
    string report_var;

    // Reporting module name. Empty means the config module reporting
    // project's own configuration.
    //
    project_name report_module;

    for (auto i (as.begin ()); i != as.end (); )
    {
      if (i->name == "null")
      {
        nullable = true;
      }
      else if (i->name == "config.report")
      {
        try
        {
          string v (i->value ? convert<string> (move (i->value)) : "true");

          if (v == "true"  ||
              v == "false" ||
              v == "multiline")
            report = move (v);
          else
            throw invalid_argument (
              "expected 'false' or format name instead of '" + v + '\'');
        }
        catch (const invalid_argument& e)
        {
          fail (as.loc) << "invalid " << i->name << " attribute value: " << e;
        }
      }
      else if (i->name == "config.report.variable")
      {
        try
        {
          report_var = convert<string> (move (i->value));

          if (!report)
            report = string ("true");
        }
        catch (const invalid_argument& e)
        {
          fail (as.loc) << "invalid " << i->name << " attribute value: " << e;
        }
      }
      else if (i->name == "config.report.module")
      {
        try
        {
          report_module = convert<project_name> (move (i->value));

          if (!report)
            report = string ("true");
        }
        catch (const invalid_argument& e)
        {
          fail (as.loc) << "invalid " << i->name << " attribute value: " << e;
        }
      }
      else
      {
        ++i;
        continue;
      }

      i = as.erase (i);
    }

    if (tt != type::word)
      fail (t) << "expected configuration variable name instead of " << t;

    string name (move (t.value));
    bool config (name.compare (0, 7, "config.") == 0);

    // As a way to print custom (discovered, computed, etc) configuration
    // information we allow specifying a non config.* variable provided it is
    // explicitly marked with the config.report attribute (or another
    // attribute that implies it).
    //
    bool new_val (false);
    string org_var; // Original variable if config.report.variable specified.

    const variable* var (nullptr); // config.* variable.
    lookup l;

    if (report && *report != "false" && !config)
    {
      if (!as.empty () || nullable)
        fail (as.loc) << "unexpected attributes for report-only variable";

      attributes_pop ();

      // Reduce to the config.report.variable-like situation.
      //
      // What should new_val be? If it's based on a config.* variable (for
      // example, some paths extracted from a tool), then it's natural for
      // that variable to control newness. And if it's not based on any
      // config.* variable, then whether it's always new or never new is a
      // philosophical question. In either case it doesn't seem useful for it
      // to unconditionally force reporting at level 2.
      //
      if (!report_var.empty ())
      {
        // For example, config [config.report.variable=multi] multi_database
        //
        org_var = move (name);
      }
      else
        report_var = move (name);

      next (t, tt); // We shouldn't have the default value part.
    }
    else
    {
      if (!report)
        report = "true"; // Default is to report.

      // Enforce the variable name pattern. The simplest is to check for the
      // config prefix and the project substring.
      //
      {
        string proj;
        {
          const project_name& n (named_project (*root_));

          if (!n.empty ())
            proj = n.variable ();
        }

        diag_record dr;
        do // Breakout loop.
        {
          if (!config)
          {
            dr << fail (t) << "configuration variable '" << name
               << "' does not start with 'config.'";
            break;
          }

          auto match = [&name] (const string& proj)
          {
            size_t p (name.find ('.' + proj));
            return (p != string::npos                        &&
                    ((p += proj.size () + 1) == name.size () || // config.<proj>
                     name[p] == '.'));                          // config.<proj>.
          };

          if (!proj.empty () && match (proj))
            break;

          // See if this buildfile belongs to a different project. If so, use
          // the project name as the reporting module name.
          //
          if (path_->path != nullptr)
          {
            // Note: all sourced/included/imported paths are absolute and
            // normalized.
            //
            const path& f (*path_->path);
            dir_path d (f.directory ());

            auto p (ctx->scopes.find (d)); // Note: never empty.
            if (*p.first != &ctx->global_scope)
            {
              // The buildfile will most likely be in src which means we may
              // end up with multiple scopes (see scope_map for background).
              // First check if one of them is us. If not, then we can extract
              // the project name from any one of them.
              //
              const scope& bs (**p.first); // Save.

              for (; p.first != p.second; ++p.first)
              {
                if (root_ == (*p.first)->root_scope ())
                  break;
              }

              if (p.first == p.second)
              {
                // Note: we expect the project itself to be named.
                //
                const project_name& n (project (*bs.root_scope ()));

                if (!n.empty ())
                {
                  // If the buildfile comes from a different project, then
                  // it's more likely to use the imported project's config
                  // variables. So replace proj with that for diagnostics
                  // below.
                  //
                  proj = n.variable ();

                  if (*report != "false" && verb >= 2)
                    report_module = n;
                }
              }
            }
            else
            {
              // If the buildfile is not in any project, then it could be
              // installed.
              //
              // Per import2_buildfile(), exported buildfiles are installed
              // into $install.buildfile/<proj>/....
              //
              const dir_path& id (build_install_buildfile);

              if (!id.empty () && d.sub (id))
              {
                dir_path l (d.leaf (id));
                if (!l.empty ())
                {
                  project_name n (*l.begin ());
                  proj = n.variable ();

                  if (*report != "false" && verb >= 2)
                    report_module = move (n);
                }
              }
            }
          }

          if (!proj.empty () && match (proj))
            break;

          // Note: only if proj not empty (see above).
          //
          if (!proj.empty ())
            dr << fail (t) << "configuration variable '" << name
               << "' does not include project name";
        }
        while (false);

        if (!dr.empty ())
          dr << info << "expected variable name in the 'config[.**]."
             << (proj.empty () ? "<project>" : proj.c_str ()) << ".**' form";
      }

      var = &parse_variable_name (move (name), get_location (t));
      apply_variable_attributes (*var);

      // Note that even though we are relying on the config.** variable
      // pattern to set global visibility, let's make sure as a sanity check.
      //
      if (var->visibility != variable_visibility::global)
      {
        fail (t) << "configuration variable " << *var << " has "
                 << var->visibility << " visibility";
      }

      // See if we have the default value part.
      //
      next (t, tt);
      bool def_val (tt != type::newline && tt != type::eos);

      if (def_val && tt != type::default_assign)
        fail (t) << "expected '?=' instead of " << t << " after "
                 << "configuration variable name";

      // If this is the special config.<project>.develop variable, verify it
      // is of type bool and has false as the default value. We also only save
      // it in config.build if it's true and suppress any unused warnings in
      // config::save_config() if specified but not used by the project.
      //
      // Here we also have the unnamed project issues (see above for details)
      // and so we actually recognize any config.**.develop.
      //
      bool dev;
      {
        size_t p (var->name.rfind ('.'));
        dev = p != 6 && var->name.compare (p + 1, string::npos, "develop") == 0;
      }

      uint64_t sflags (0);
      if (dev)
      {
        if (var->type != &value_traits<bool>::value_type)
          fail (loc) << *var << " variable must be of type bool";

        // This is quite messy: below we don't always parse the value (plus it
        // may be computed) so here we just peek at the next token. But we
        // have to do this in the same mode as parse_variable_value().
        //
        if (!def_val                                    ||
            peek (lexer_mode::value, '@') != type::word ||
            peeked ().value != "false")
          fail (loc) << *var << " variable default value must be literal false";

        if (nullable)
          fail (loc) << *var << " variable must not be nullable";

        sflags |= config::save_false_omitted;
      }

      // We have to lookup the value whether we have the default part or not
      // in order to mark it as saved. We also have to do this to get the new
      // value status.
      //
      l = config::lookup_config (new_val, *root_, *var, sflags);

      // Handle the default value.
      //
      if (def_val)
      {
        // The rest is the default value which we should parse in the value
        // mode. But before switching check whether we need to evaluate it at
        // all.
        //
        if (l.defined ())
        {
          // Peek at the attributes to detect whether the value is NULL.
          //
          if (!dev && !nullable)
          {
            // Essentially a prefix of parse_variable_value().
            //
            mode (lexer_mode::value, '@');
            next_with_attributes (t, tt);
            attributes_push (t, tt, true);
            for (const attribute& a: attributes_pop ())
            {
              if (a.name == "null")
              {
                nullable = true;
                break;
              }
            }
          }

          skip_line (t, tt);
        }
        else
        {
          value lhs, rhs (parse_variable_value (t, tt, !dev /* mode */));
          apply_value_attributes (var, lhs, move (rhs), type::assign);

          if (!nullable)
            nullable = lhs.null;

          l = config::lookup_config (new_val, *root_, *var, move (lhs), sflags);
        }
      }

      // If the variable is not nullable, verify the value is not NULL.
      //
      // Note that undefined is not the same as NULL (if it is undefined, we
      // should either see the default value or if there is no default value,
      // then the user is expected to handle the undefined case).
      //
      if (!nullable && l.defined () && l->null)
        fail (loc) << "null value in non-nullable variable " << *var;
    }

    // We will be printing the report at either level 2 (-v) or 3 (-V)
    // depending on the final value of config_report::new_value.
    //
    // Note that for the config_report::new_value calculation we only
    // incorporate variables that we are actually reporting.
    //
    if (*report != "false" && verb >= 2)
    {
      // Find existing or insert new config_report entry for this module.
      //
      auto i (find_if (config_reports.begin (),
                       config_reports.end (),
                       [&report_module] (const config_report& r)
                       {
                         return r.module == report_module;
                       }));

      if (i == config_reports.end ())
      {
        config_reports.push_back (
          config_report {move (report_module), {}, false});
        i = config_reports.end () - 1;
      }

      auto& report_values (i->values);
      bool& report_new_value (i->new_value);

      // We don't want to lookup the report variable value here since it's
      // most likely not set yet.
      //
      if (!report_var.empty ())
      {
        if (org_var.empty () && var != nullptr)
          org_var = var->name;

        // In a somewhat hackish way we pass the variable in an undefined
        // lookup.
        //
        // Note: consistent with parse_variable_name() wrt overridability.
        //
        l = lookup ();
        l.var = &root_->var_pool ().insert (
          move (report_var),
          report_var.find ('.') != string::npos /* overridable */);
      }

      if (l.var != nullptr)
      {
        // If we have a duplicate, update it (it could be useful to have
        // multiple config directives to "probe" the value before calculating
        // the default; see lookup_config() for details).
        //
        // Since the original variable is what the user will see in the
        // report, we prefer that as a key.
        //
        auto i (find_if (report_values.begin (),
                         report_values.end (),
                         [&org_var, &l] (const config_report::value& v)
                         {
                           return (v.org.empty () && org_var.empty ()
                                   ? v.val.var == l.var
                                   : (v.org.empty ()
                                      ? v.val.var->name == org_var
                                      : v.org == l.var->name));
                         }));

        if (i == report_values.end ())
          report_values.push_back (
            config_report::value {l, move (*report), move (org_var)});
        else
        {
          i->val = l;
          i->fmt = move (*report);
          if (i->org.empty ()) i->org = move (org_var);
        }

        report_new_value = report_new_value || new_val;
      }
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_config_environment (token& t, type& tt)
  {
    // config.environment <name>...
    //

    // While we could allow this directive during bootstrap, it would have to
    // be after loading the config module, which can be error prone. So we
    // disallow it for now (it's also not clear "configuring" bootstrap with
    // environment variables is a good idea; think of info, etc).
    //
    if (stage_ == stage::boot)
      fail (t) << "config.environment during bootstrap";

    // Parse the rest as names in the value mode to get variable expansion,
    // etc.
    //
    mode (lexer_mode::value);
    next (t, tt);
    const location l (get_location (t));

    strings ns;
    try
    {
      ns = convert<strings> (
        tt != type::newline && tt != type::eos
        ? parse_names (t, tt,
                       pattern_mode::ignore,
                       "environment variable name",
                       nullptr)
        : names ());
    }
    catch (const invalid_argument& e)
    {
      fail (l) << "invalid environment variable name: " << e.what ();
    }

    config::save_environment (*root_, ns);

    next_after_newline (t, tt);
  }

  void parser::
  parse_import (token& t, type& tt)
  {
    tracer trace ("parser::parse_import", &path_);

    if (stage_ == stage::boot)
      fail (t) << "import during bootstrap";

    // General import form:
    //
    // import[?!] [<attrs>] <var> = [<attrs>] (<target>|<project>%<target>])+
    //
    // Special form for importing buildfiles:
    //
    // import[?!] [<attrs>] (<target>|<project>%<target>])+
    //
    bool opt (t.value.back () == '?');
    optional<string> ph2 (opt || t.value.back () == '!'
                          ? optional<string> (string ())
                          : nullopt);

    // We are now in the normal lexing mode and we let the lexer handle `=`.
    //
    next_with_attributes (t, tt);

    // Get variable (or value, in the second form) attributes, if any, and
    // deal with the special metadata and rule_hint attributes. Since
    // currently they can only appear in the import directive, we handle them
    // in an ad hoc manner.
    //
    attributes_push (t, tt);

    bool meta (false); // Import with metadata.
    bool once (false); // Import buildfile once.
    bool nodt (false); // Import buildfile without default target semantics.
    {
      attributes& as (attributes_top ());
      const location& l (as.loc);

      for (auto i (as.begin ()); i != as.end (); )
      {
        const string& n (i->name);
        value& v (i->value);

        if (n == "metadata")
        {
          if (!ph2)
            fail (l) << "loading metadata requires immediate import" <<
              info << "consider using the import! directive instead";

          meta = true;
        }
        else if (n == "no_default_target")
        {
          nodt = true;
        }
        else if (n == "once")
        {
          once = true;
        }
        else if (n == "rule_hint")
        {
          if (!ph2)
            fail (l) << "rule hint can only be used with immediate import" <<
              info << "consider using the import! directive instead";

          // Here we only allow a single name.
          //
          try
          {
            ph2 = convert<string> (move (v));

            if (ph2->empty ())
              throw invalid_argument ("empty name");
          }
          catch (const invalid_argument& e)
          {
            fail (l) << "invalid " << n << " attribute value: " << e;
          }
        }
        else
        {
          ++i;
          continue;
        }

        i = as.erase (i);
      }
    }

    // Note that before supporting the second form (without <var>) we used to
    // parse the value after assignment in the value mode. However, we don't
    // really need to since what we should have is a bunch of target names.
    // In other words, whatever the value mode does not treat as special
    // compared to the normal mode (like `:`) would be illegal here.
    //
    // Note that we expant patterns for the ad hoc import case:
    //
    // import sub = */
    //
    // @@ PAT: the only issue here is that we currently pattern-expand var
    //         name (same assue as with target-specific var names).
    //
    if (!start_names (tt))
      fail (t) << "expected variable name or buildfile target instead of " << t;

    location loc (get_location (t));
    names ns (parse_names (t, tt, pattern_mode::expand));

    // Next could come the assignment operator. Note that we don't support
    // default assignment (?=) yet (could make sense when attempting to import
    // alternatives or some such).
    //
    type atype;
    const variable* var (nullptr);
    if (tt == type::assign || tt == type::append || tt == type::prepend)
    {
      var = &parse_variable_name (move (ns), loc);
      apply_variable_attributes (*var);

      if (var->visibility > variable_visibility::scope)
      {
        fail (loc) << "variable " << *var << " has " << var->visibility
                   << " visibility but is assigned in import";
      }

      atype = tt;
      next_with_attributes (t, tt);
      attributes_push (t, tt, true /* standalone */);

      if (!start_names (tt))
        fail (t) << "expected target to import instead of " << t;

      loc = get_location (t);
      ns = parse_names (t, tt, pattern_mode::expand);
    }
    else if (tt == type::default_assign)
      fail (t) << "default assignment not yet supported";


    // If there are any value attributes, roundtrip the names through the
    // value applying the attributes.
    //
    if (!attributes_top ().empty ())
    {
      value lhs, rhs (move (ns));
      apply_value_attributes (nullptr, lhs, move (rhs), type::assign);

      if (!lhs)
        fail (loc) << "expected target to import instead of null value";

      untypify (lhs, true /* reduce */);
      ns = move (lhs.as<names> ());
    }
    else
      attributes_pop ();

    value* val (var != nullptr ?
                &(atype == type::assign
                  ? scope_->assign (*var)
                  : scope_->append (*var))
                : nullptr);

    for (name& n: ns)
    {
      // @@ Could this be an out-qualified ad hoc import? Yes, see comment
      //    about buildfile import in import_load().
      //
      if (n.pair)
        fail (loc) << "unexpected pair in import";

      // See if we are importing a buildfile target. Such an import is always
      // immediate.
      //
      bool bf (n.type == "buildfile");
      if (bf)
      {
        if (meta)
          fail (loc) << "metadata requested for buildfile target " << n;

        if (var != nullptr)
        {
          if (once)
            fail (loc) << "once importation requested with variable assignment";

          if (nodt)
            fail (loc) << "no_default_target importation requested with "
                       << "variable assignment";
        }

        if (ph2 && !ph2->empty ())
          fail (loc) << "rule hint specified for buildfile target " << n;
      }
      else
      {
        if (once)
          fail (loc) << "once importation requested for target " << n;

        if (nodt)
          fail (loc) << "no_default_target importation requested for target "
                     << n;

        if (var == nullptr)
          fail (loc) << "variable assignment required to import target " << n;
      }

      // import() will check the name, if required.
      //
      import_result<scope> ir (
        import (*scope_,
                move (n),
                ph2 ? ph2 : bf ? optional<string> (string ()) : nullopt,
                opt,
                meta,
                loc));

      names& r (ir.name);

      if (val != nullptr)
      {
        if (r.empty ()) // Optional not found.
        {
          if (atype == type::assign)
            *val = nullptr;
        }
        else
        {
          // Import (more precisely, alias) the target type into this project
          // if not known.
          //
          // Note that if the result is ignored (val is NULL), then it's fair
          // to assume this is not necessary.
          //
          if (const scope* iroot = ir.target)
          {
            const name& n (r.front ());
            if (n.typed ())
              import_target_type (*root_, *iroot, n.type, loc);
          }

          if      (atype == type::assign)  val->assign  (move (r), var);
          else if (atype == type::prepend) val->prepend (move (r), var);
          else                             val->append  (move (r), var);
        }

        if (atype == type::assign)
          atype = type::append; // Append subsequent values.
      }
      else
      {
        assert (bf);

        if (r.empty ()) // Optional not found.
        {
          assert (opt);
          continue;
        }

        // Note: see also import_buildfile().
        //
        assert (r.size () == 1); // See import_load() for details.
        name& n (r.front ());
        path p (n.dir / n.value); // Should already include extension.

        // Note: similar to parse_include().
        //
        // Nuance: we insert this buildfile even with once=false in case it
        // gets imported with once=true from another place.
        //
        if (!root_->root_extra->insert_buildfile (p) && once)
        {
          l5 ([&]{trace (loc) << "skipping already imported " << p;});
          continue;
        }

        // Clear/restore if/switch location.
        //
        auto g = make_guard ([this, old = condition_] () mutable
                             {
                               condition_ = old;
                             });
        condition_ = nullopt;

        try
        {
          ifdstream ifs (p);

          auto df = make_diag_frame (
            [this, &p, &loc] (const diag_record& dr)
            {
              dr << info (loc) << p << " imported from here";
            });

          // @@ Do we want to enter this buildfile? What's the harm (one
          //    benefit is that it will be in dump). But, we currently don't
          //    out-qualify them, though feels like there is nothing fatal
          //    in that, just inaccurate.
          //
          source_buildfile (ifs,
                            path_name (p),
                            loc,
                            nodt ? optional<bool> {} : false);
        }
        catch (const io_error& e)
        {
          fail (loc) << "unable to read imported buildfile " << p << ": " << e;
        }
      }
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_export (token& t, type& tt)
  {
    tracer trace ("parser::parse_export", &path_);

    scope* ps (scope_->parent_scope ());

    // This should be temp_scope.
    //
    if (ps == nullptr || ps->out_path () != scope_->out_path ())
      fail (t) << "export outside export stub";

    // The rest is a value. Parse it similar to a value on the RHS of an
    // assignment to get expansion. While it may seem like supporting
    // attributes is a good idea here, there is actually little benefit in
    // being able to type them or to return NULL.
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    auto at (attributes_push (t, tt));

    if (at.first)
      fail (at.second) << "attributes in export";
    else
      attributes_pop ();

    location l (get_location (t));
    value val (tt != type::newline && tt != type::eos
               ? parse_value (t, tt, pattern_mode::expand)
               : value (names ()));

    if (!val)
      fail (l) << "null value in export";

    if (val.type != nullptr)
    {
      // While feels far-fetched, let's preserve empty typed values in the
      // result.
      //
      untypify (val, false /* reduce */);
    }

    export_value = move (val).as<names> ();

    if (export_value.empty ())
      fail (l) << "empty value in export";

    next_after_newline (t, tt);
  }

  void parser::
  parse_using (token& t, type& tt)
  {
    tracer trace ("parser::parse_using", &path_);

    bool opt (t.value.back () == '?');

    if (opt && stage_ == stage::boot)
      fail (t) << "optional module in bootstrap";

    // The rest should be a list of module names. Parse them as names in the
    // value mode to get variable expansion, etc.
    //
    mode (lexer_mode::value, '@');
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::ignore, "module", nullptr)
              : names ());

    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      string n;
      optional<standard_version> v;

      if (!i->simple ())
        fail (l) << "expected module name instead of " << *i;

      n = move (i->value);

      if (n[0] == '_')
        fail (l) << "module name '" << n << "' starts with underscore";

      if (i->pair)
      try
      {
        if (i->pair != '@')
          fail (l) << "unexpected pair style in using directive";

        ++i;
        if (!i->simple ())
          fail (l) << "expected module version instead of " << *i;

        v = standard_version (i->value, standard_version::allow_earliest);
      }
      catch (const invalid_argument& e)
      {
        fail (l) << "invalid module version '" << i->value << "': " << e;
      }

      // Handle the special 'build' and 'build2' modules.
      //
      if (n == "build2" || n == "build")
      {
        if (v)
        {
          standard_version_constraint c (move (v), false, nullopt, true); // >=
          check_build_version (c, l);
        }
      }
      else
      {
        assert (!v); // Module versioning not yet implemented.

        if (stage_ == stage::boot)
          boot_module (*root_, n, l);
        else
          init_module (*root_, *scope_, n, l, opt);
      }
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_define (token& t, type& tt)
  {
    // define [<attrs>] <derived>: <base>
    // define <alias> = <scope>/<type>
    //
    // See tests/define.
    //
    next_with_attributes (t, tt);

    attributes_push (t, tt);
    attributes as (attributes_pop ());

    if (tt != type::word)
      fail (t) << "expected name instead of " << t << " in target type "
               << "definition";

    string n (move (t.value));
    const location nl (get_location (t));

    next (t, tt);

    if (tt == type::colon)
    {
      // Handle attributes.
      //
      target_type::flag fs (target_type::flag::none);
      {
        const location& l (as.loc);

        for (attribute& a: as)
        {
          const string& n (a.name);
          value& v (a.value);

          if      (n == "see_through") fs |= target_type::flag::see_through;
          else if (n == "member_hint") fs |= target_type::flag::member_hint;
          else
            fail (l) << "unknown target type definition attribute " << n;

          if (!v.null)
            fail (l) << "unexpected value in attribute " << n;
        }
      }

      if (next (t, tt) != type::word)
        fail (t) << "expected name instead of " << t << " in target type "
                 << "definition";

      // Target.
      //
      const string& bn (t.value);
      const target_type* bt (scope_->find_target_type (bn));

      if (bt == nullptr)
        fail (t) << "unknown target type " << bn <<
          info << "perhaps the module that defines this target type is "
               << "not loaded by project " << *scope_->root_scope ();

      // The derive_target_type() call below does not produce a non-abstract
      // type if passed an abstract base. So we ban this for now (it's unclear
      // why would someone want to do this).
      //
      if (bt->factory == nullptr)
        fail (t) << "abstract base target type " << bt->name << "{}";

      // Note that the group{foo}<...> syntax is only recognized for group-
      // based targets and ad hoc buildscript recipes/rules only match group.
      // (We may want to relax this for member_hint in the future since its
      // currently also used on non-mtime-based targets, though what exactly
      // we will do in ad hoc recipes/rules in this case is fuzzy).
      //
      if ((fs & target_type::flag::group) == target_type::flag::group &&
          !bt->is_a<group> ())
        fail (t) << "base target type " << bn << " must be group for "
                 << "group-related attribute";

      if (!root_->derive_target_type (move (n), *bt, fs).second)
        fail (nl) << "target type " << n << " already defined in project "
                  << *root_;

      next (t, tt); // Get newline.
    }
    else if (tt == type::assign)
    {
      if (!as.empty ())
        fail (as.loc) << "unexpected target type alias attribute";

      // The rest should be a path-like target type. Parse it as names in
      // the value mode to get variable expansion, etc.
      //
      mode (lexer_mode::value, '@');
      next (t, tt);
      const location tl (get_location (t));
      names ns (
        parse_names (t, tt, pattern_mode::ignore, "target type", nullptr));

      name* tn (nullptr);
      if (ns.size () == 1)
      {
        tn = &ns.front ();

        if (tn->file ())
        {
          try
          {
            tn->canonicalize ();

            if (tn->dir.absolute ())
              tn->dir.normalize ();
            else
              tn = nullptr;
          }
          catch (const invalid_path&) {tn = nullptr;}
          catch (const invalid_argument&) {tn = nullptr;}
        }
        else
          tn = nullptr;
      }

      if (tn == nullptr)
        fail (tl) << "expected scope-qualified target type instead of " << ns;

      // If we got here, then tn->dir is the scope and tn->value is the target
      // type.
      //
      // NOTE: see similar code in import_target_type().
      //
      const target_type* tt (nullptr);
      if (const scope* rs = ctx->scopes.find_out (tn->dir).root_scope ())
      {
        tt = rs->find_target_type (tn->value);

        if (tt == nullptr)
          fail (tl) << "unknown target type " << tn->value << " in scope "
                    << *rs;
      }
      else
        fail (tl) << "unknown project scope " << tn->dir << " in scope"
                  << "-qualified target type" <<
          info << "did you forget to import the corresponding project?";

      if (n != tn->value)
        fail (nl) << "alias target type name " << n << " does not match "
                  << tn->value;

      // Note that this is potentially a shallow reference to a user-derived
      // target type. Seeing that we only ever destory the entire graph, this
      // should be ok.
      //
      auto p (root_->root_extra->target_types.insert (*tt));

      if (!p.second && &p.first.get () != tt)
        fail (nl) << "target type " << n << " already defined in this project";
    }
    else
      fail (t) << "expected ':' or '=' instead of " << t << " in target type "
               << "definition";

    next_after_newline (t, tt);
  }

  void parser::
  parse_if_else (token& t, type& tt)
  {
    auto g = make_guard ([this, old = condition_] () mutable
                         {
                           condition_ = old;
                         });
    condition_ = get_location (t);

    parse_if_else (t, tt,
                   false /* multi */,
                   [this] (token& t, type& tt, bool s, const string& k)
                   {
                     return parse_clause_block (t, tt, s, k);
                   },
                   {});
  }

  void parser::
  parse_if_else (token& t, type& tt,
                 bool multi,
                 const function<void (
                   token&, type&, bool, const string&)>& parse_block,
                 const function<void (
                   token&, token_type&, const string&)>& parse_recipe_directive)
  {
    // Handle the whole if-else chain. See tests/if-else.
    //
    bool taken (false); // One of the branches has been taken.

    for (;;)
    {
      string k (move (t.value));

      next_with_attributes (t, tt); // Recognize attributes before value.

      bool take (false); // Take this branch?

      if (k != "else")
      {
        // Should we evaluate the expression if one of the branches has
        // already been taken? On the one hand, evaluating it is a waste
        // of time. On the other, it can be invalid and the only way for
        // the user to know their buildfile is valid is to test every
        // branch. There could also be side effects. We also have the same
        // problem with ignored branch blocks except there evaluating it
        // is not an option. So let's skip it.
        //
        if (taken)
          skip_line (t, tt); // Skip expression.
        else
        {
          if (tt == type::newline || tt == type::eos)
            fail (t) << "expected " << k << "-expression instead of " << t;

          // Parse the condition similar to a value on the RHS of an
          // assignment (expansion, attributes). While at this stage the
          // attribute's usefulness in this context is not entirely clear, we
          // allow it for consistency with other similar directives (switch,
          // for) and also who knows what attributes we will have in the
          // future (maybe there will be a way to cast 0/[null] to bool, for
          // example).
          //
          // Note also that we expand patterns (they could be used in nested
          // contexts, etc; e.g., "if pattern expansion is empty" condition).
          //
          const location l (get_location (t));

          try
          {
            // Should evaluate to 'true' or 'false'.
            //
            bool e (
              convert<bool> (
                parse_value_with_attributes (t, tt,
                                             pattern_mode::expand,
                                             "expression",
                                             nullptr)));

            take = (k.back () == '!' ? !e : e);
          }
          catch (const invalid_argument& e) { fail (l) << e; }
        }
      }
      else
        take = !taken;

      if (tt != type::newline)
        fail (t) << "expected newline instead of " << t << " after " << k
                 << (k != "else" ? "-expression" : "");

      // This can be a block (single or multi-curly) or a single line. The
      // single-curly block is a bit tricky, consider:
      //
      // else
      //   {hxx cxx}{options}: install = false
      //
      // So we treat it as a block if it's followed immediately by newline.
      //
      // Note: identical code in parse_switch().
      //
      next (t, tt);
      if (multi
          ? (tt == type::multi_lcbrace)
          : (tt == type::lcbrace && peek () == type::newline))
      {
        parse_block (t, tt, !take, k);
        taken = taken || take;
      }
      else
      {
        // The only valid line in multi-curly if-else is `recipe`.
        //
        if (multi)
        {
          // Note that we cannot do the keyword test if we are replaying. So
          // we skip it with the understanding that if it's not a keywords,
          // then we wouldn't have gotten here on the replay.
          //
          if (tt == type::word &&
              (replay_ == replay::play || keyword (t)) &&
              t.value == "recipe")
          {
            if (take)
            {
              parse_recipe_directive (t, tt, k);
              taken = true;
            }
            else
            {
              skip_line (t, tt);

              if (tt == type::newline)
                next (t, tt);
            }
          }
          else
            fail (t) << "expected " << k << "-block or 'recipe' instead of "
                     << t;
        }
        else
        {
          if (tt == type::multi_lcbrace)
            fail (t) << "expected " << k << "-line instead of " << t <<
              info << "did you forget to specify % recipe header?";

          if (take)
          {
            if (!parse_clause (t, tt, true))
              fail (t) << "expected " << k << "-line instead of " << t;

            taken = true;
          }
          else
          {
            skip_line (t, tt);

            if (tt == type::newline)
              next (t, tt);
          }
        }
      }

      // See if we have another el* keyword.
      //
      // Note that we cannot do the keyword test if we are replaying. So we
      // skip it with the understanding that if it's not a keywords, then we
      // wouldn't have gotten here on the replay (see parse_recipe() for
      // details).
      //
      if (k != "else"      &&
          tt == type::word &&
          (replay_ == replay::play || keyword (t)))
      {
        const string& n (t.value);

        if (n == "else" || n == "elif" || n == "elif!")
          continue;
      }

      break;
    }
  }

  void parser::
  parse_switch (token& t, type& tt)
  {
    auto g = make_guard ([this, old = condition_] () mutable
                         {
                           condition_ = old;
                         });
    condition_ = get_location (t);

    parse_switch (t, tt,
                  false /* multi */,
                  [this] (token& t, type& tt, bool s, const string& k)
                  {
                    return parse_clause_block (t, tt, s, k);
                  },
                  {});
  }

  void parser::
  parse_switch (token& t, type& tt,
                bool multi,
                const function<void (
                  token&, type&, bool, const string&)>& parse_block,
                const function<void (
                  token&, token_type&, const string&)>& parse_recipe_directive)
  {
    // switch <value> [: <func> [<arg>]] [, <value>...]
    // {
    //   case <pattern> [, <pattern>...]
    //     <line>
    //
    //   case <pattern> [, <pattern>...]
    //   {
    //     <block>
    //   }
    //
    //   case <pattern> [, <pattern>...]
    //   ...
    //   case <pattern> [, <pattern>...]
    //     ...
    //
    //   case <pattern> [| <pattern>... ]
    //
    //   default
    //     ...
    // }

    assert (!pre_parse_); // Used to skip pattern alternatives.

    // Parse and evaluate the values we are matching. Similar to if-else, we
    // expand patterns.
    //
    struct expr
    {
      build2::value    value;
      optional<string> func;
      names            arg;
    };
    small_vector<expr, 1> exprs;

    mode (lexer_mode::switch_expressions); // Recognize `:` and `,`.

    do
    {
      next_with_attributes (t, tt); // Recognize attributes before value.

      if (tt == type::newline || tt == type::eos)
        fail (t) << "expected switch expression instead of " << t;

      expr e;

      e.value =
        parse_value_with_attributes (t, tt,
                                     pattern_mode::expand,
                                     "expression",
                                     nullptr);

      if (tt == type::colon)
      {
        next (t, tt);
        const location l (get_location (t));
        names ns (parse_names (t, tt, pattern_mode::preserve, "function name"));

        if (ns.empty () || ns[0].empty ())
          fail (l) << "function name expected after ':'";

        if (ns[0].pattern || !ns[0].simple ())
          fail (l) << "function name expected instead of " << ns[0];

        e.func = move (ns[0].value);
        ns.erase (ns.begin ());
        e.arg = move (ns);
      }

      exprs.push_back (move (e));
    }
    while (tt == type::comma);

    next_after_newline (t, tt, "switch expression");

    // Next we should always have a block.
    //
    if (tt != type::lcbrace)
      fail (t) << "expected '{' instead of " << t << " after switch";

    next (t, tt);
    next_after_newline (t, tt, '{');

    // Next we have zero or more `case` lines/blocks (potentially with
    // multiple `case`s per line/block) optionally followed by the `default`
    // lines/blocks followed by the closing `}`.
    //
    bool taken (false); // One of the case/default has been taken.
    bool seen_default (false);

    auto special = [&seen_default, this] (const token& t, const type& tt)
    {
      // Note that we cannot do the keyword test if we are replaying. So we
      // skip it with the understanding that if it's not a keywords, then we
      // wouldn't have gotten here on the replay (see parse_recipe() for
      // details). Note that this appears to mean that replay cannot be used
      // if we allow lines, only blocks. Consider:
      //
      // case ...
      //  case = x
      //
      // (We don't seem to have the same problem with if-else because there we
      // always expect one line for if/else.)
      //
      // Idea: maybe we could save the result of the keyword test in a token
      // to be replayed? (For example, if we ever decided to allow if-else and
      // switch in variable blocks.)
      //
      if (tt == type::word && (replay_ == replay::play || keyword (t)))
      {
        if (t.value == "case")
        {
          if (seen_default)
            fail (t) << "case after default" <<
              info << "default must be last in the switch block";

          return true;
        }
        else if (t.value == "default")
        {
          if (seen_default)
            fail (t) << "multiple defaults";

          seen_default = true;
          return true;
        }
        // Fall through.
      }

      return false;
    };

    while (tt != type::eos)
    {
      if (tt == type::rcbrace)
        break;

      if (!special (t, tt))
        fail (t) << "expected case or default instead of " << t;

      string k (move (t.value));

      bool take (false); // Take this case/default?
      if (seen_default)
      {
        take = !taken;
        next (t, tt);
      }
      else
      {
        // Similar to if-else we are not going to evaluate the case conditions
        // if we are skipping.
        //
        if (taken)
          skip_line (t, tt);
        else
        {
          // Parse the patterns and match them against the values. Note that
          // here we don't expand patterns in names.
          //
          mode (lexer_mode::case_patterns); // Recognize `|` and `,`.

          auto parse_pattern_with_attributes = [this] (token& t, type& tt)
          {
            return parse_value_with_attributes (
              t, tt, pattern_mode::ignore, "pattern", nullptr);
          };

          for (size_t i (0);; ++i)
          {
            // Recognize attributes before first pattern.
            //
            next_with_attributes (t, tt);

            if (tt == type::newline || tt == type::eos)
              fail (t) << "expected case pattern instead of " << t;

            if (i == exprs.size ())
              fail (t) << "more patterns than switch expressions";

            // Handle pattern alternatives (<pattern>|<pattern>).
            //
            for (;;)
            {
              const location l (get_location (t));
              value p (parse_pattern_with_attributes (t, tt));
              expr& e (exprs[i]); // Note: value might be modified (typified).

              if (e.func)
              {
                // Call <func>(<value>, <pattern> [, <arg>]).
                //
                small_vector<value, 3> args {value (e.value), move (p)};

                if (!e.arg.empty ())
                  args.push_back (value (e.arg));

                value r (ctx->functions.call (scope_, *e.func, args, l));

                // We support two types of functions: matchers and extractors:
                // a matcher returns a statically-typed bool value while an
                // extractor returns NULL if there is no match and the
                // extracted value otherwise.
                //
                if (r.type == &value_traits<bool>::value_type)
                {
                  if (r.null)
                    fail (l) << "match function " << *e.func << " returned "
                             << "null";

                  take = r.as<bool> ();
                }
                else
                  take = !r.null;
              }
              else
                take = compare_values (type::equal, e.value, p, l);

              if (tt != type::bit_or)
                break;

              if (take)
              {
                // Use the pre-parse mechanism to skip remaining alternatives.
                //
                pre_parse_ = true;
                do
                {
                  next_with_attributes (t, tt); // Skip `|`.
                  parse_pattern_with_attributes (t, tt);
                }
                while (tt == type::bit_or);
                pre_parse_ = false;

                break;
              }

              // Recognize attributes before next pattern.
              //
              next_with_attributes (t, tt);
            }

            if (!take)
            {
              skip_line (t, tt); // Skip the rest.
              break;
            }

            // We reserve the ':' separator for possible future match
            // extraction support:
            //
            // case '...': x
            //   info "$x"
            //
            if (tt == type::colon)
              fail (t) << "unexpected ':' (match extraction is not yet "
                       << "supported)";

            if (tt != type::comma)
              break;
          }
        }
      }

      next_after_newline (t, tt, seen_default ? "default" : "case pattern");

      // This can be another `case` or `default`.
      //
      if (special (t, tt))
      {
        // If we are still looking for a match, simply restart from the
        // beginning as if this were the first `case` or `default`.
        //
        if (!take && !taken)
        {
          seen_default = false;
          continue;
        }

        // Otherwise, we need to skip this and all the subsequent special
        // lines.
        //
        do
        {
          skip_line (t, tt);
          next_after_newline (t, tt, seen_default ? "default" : "case pattern");
        }
        while (special (t, tt));
      }

      // Otherwise this must be a block (single or multi-curly) or a single
      // line (the same logic as in if-else).
      //
      if (multi
          ? (tt == type::multi_lcbrace)
          : (tt == type::lcbrace && peek () == type::newline))
      {
        parse_block (t, tt, !take, k);
        taken = taken || take;
      }
      else
      {
        if (multi)
        {
          if (tt == type::word &&
              (replay_ == replay::play || keyword (t)) &&
              t.value == "recipe")
          {
            if (take)
            {
              parse_recipe_directive (t, tt, k);
              taken = true;
            }
            else
            {
              skip_line (t, tt);

              if (tt == type::newline)
                next (t, tt);
            }
          }
          else
            fail (t) << "expected " << k << "-block or 'recipe' instead of "
                     << t;
        }
        else
        {
          if (take)
          {
            if (!parse_clause (t, tt, true))
              fail (t) << "expected " << k << "-line instead of " << t;

            taken = true;
          }
          else
          {
            skip_line (t, tt);

            if (tt == type::newline)
              next (t, tt);
          }
        }
      }
    }

    if (tt != type::rcbrace)
      fail (t) << "expected '}' instead of " << t << " after switch-block";

    next (t, tt);                    // Presumably newline after '}'.
    next_after_newline (t, tt, '}'); // Should be on its own line.
  }

  void parser::
  parse_for (token& t, type& tt)
  {
    // for [<var-attrs>] <varname> [<elem-attrs>]: [<val-attrs>] <value>
    //   <line>
    //
    // for [<var-attrs>] <varname> [<elem-attrs>]: [<val-attrs>] <value>
    // {
    //   <block>
    // }

    // First take care of the variable name. There is no reason not to
    // support variable attributes.
    //
    next_with_attributes (t, tt);
    attributes_push (t, tt);

    // Enable list element attributes.
    //
    enable_attributes ();

    const location vloc (get_location (t));
    names vns (parse_names (t, tt, pattern_mode::preserve));

    const variable& var (parse_variable_name (move (vns), vloc));
    apply_variable_attributes (var);

    if (var.visibility > variable_visibility::scope)
    {
      fail (vloc) << "variable " << var << " has " << var.visibility
                  << " visibility but is assigned in for-loop";
    }

    // Parse the list element attributes, if present.
    //
    attributes_push (t, tt);

    if (tt != type::colon)
      fail (t) << "expected ':' instead of " << t << " after variable name";

    // Save element attributes so that we can inject them on each iteration.
    //
    attributes val_attrs (attributes_pop ());

    // Now the value (list of names) to iterate over. Parse it similar to a
    // value on the RHS of an assignment (expansion, attributes).
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    value val (parse_value_with_attributes (t, tt, pattern_mode::expand));

    // If the value type provides custom iterate function, then use that (see
    // value_type::iterate for details).
    //
    auto iterate (val.type != nullptr ? val.type->iterate : nullptr);

    // If this value is a container, then save its element type so that we
    // can typify each element below.
    //
    const value_type* etype (nullptr);

    if (!iterate && val && val.type != nullptr)
    {
      etype = val.type->element_type;

      // Note that here we don't want to be reducing empty simple values to
      // empty lists.
      //
      untypify (val, false /* reduce */);
    }

    if (tt != type::newline)
      fail (t) << "expected newline instead of " << t << " after for";

    // Finally the body. The initial thought was to use the token replay
    // facility but on closer inspection this didn't turn out to be a good
    // idea (no support for nested replays, etc). So instead we are going to
    // do a full-blown re-lex. Specifically, we will first skip the line/block
    // just as we do for non-taken if/else branches while saving the character
    // sequence that comprises the body. Then we re-lex/parse it on each
    // iteration.
    //
    string body;
    uint64_t line (lexer_->line); // Line of the first character to be saved.
    lexer::save_guard sg (*lexer_, body);

    // This can be a block or a single line, similar to if-else.
    //
    bool block (next (t, tt) == type::lcbrace && peek () == type::newline);

    if (block)
    {
      next (t, tt); // Get newline.
      next (t, tt);

      skip_block (t, tt);
      sg.stop ();

      if (tt != type::rcbrace)
        fail (t) << "expected '}' instead of " << t << " at the end of "
                 << "for-block";

      next (t, tt);                    // Presumably newline after '}'.
      next_after_newline (t, tt, '}'); // Should be on its own line.
    }
    else
    {
      skip_line (t, tt);
      sg.stop ();

      if (tt == type::newline)
        next (t, tt);
    }

    // Iterate.
    //
    value& lhs (scope_->assign (var)); // Assign even if no iterations.

    if (!val)
      return;

    names* ns (nullptr);
    if (!iterate)
    {
      ns = &val.as<names> ();
      if (ns->empty ())
        return;
    }

    istringstream is (move (body));

    struct data
    {
      const variable&   var;
      const attributes& val_attrs;
      uint64_t          line;
      bool              block;
      value&            lhs;
      istringstream&    is;

    } d {var, val_attrs, line, block, lhs, is};

    function<void (value&&, bool first)> iteration =
      [this, &d] (value&& v, bool first)
    {
      // Rewind the stream.
      //
      if (!first)
      {
        d.is.clear ();
        d.is.seekg (0);
      }

      // Inject element attributes.
      //
      attributes_.push_back (d.val_attrs);

      apply_value_attributes (&d.var, d.lhs, move (v), type::assign);

      lexer l (d.is, *path_, d.line);
      lexer* ol (lexer_);
      lexer_ = &l;

      token t;
      type tt;
      next (t, tt);

      if (d.block)
      {
        next (t, tt); // {
        next (t, tt); // <newline>
      }

      parse_clause (t, tt);

      if (tt != (d.block ? type::rcbrace : type::eos))
        fail (t) << "expected name " << (d.block ? "or '}' " : "")
                 << "instead of " << t;

      lexer_ = ol;
    };

    if (!iterate)
    {
      for (auto b (ns->begin ()), i (b), e (ns->end ()); i != e; ++i)
      {
        // Set the variable value.
        //
        bool pair (i->pair);
        names n;
        n.push_back (move (*i));
        if (pair) n.push_back (move (*++i));
        value v (move (n));

        if (etype != nullptr)
          typify (v, *etype, &var);

        iteration (move (v), i == b);
      }
    }
    else
      iterate (val, iteration);
  }

  void parser::
  parse_assert (token& t, type& tt)
  {
    bool neg (t.value.back () == '!');
    const location al (get_location (t));

    // Parse the next chunk (the condition) similar to a value on the RHS of
    // an assignment. We allow attributes (which will only apply to the
    // condition) for the same reason as in if-else (see parse_if_else()).
    //
    mode (lexer_mode::value);
    next_with_attributes (t, tt);

    const location el (get_location (t));

    try
    {
      // Should evaluate to 'true' or 'false'.
      //
      bool e (
        convert<bool> (
          parse_value_with_attributes (t, tt,
                                       pattern_mode::expand,
                                       "expression",
                                       nullptr,
                                       true /* chunk */)));
      e = (neg ? !e : e);

      if (e)
      {
        skip_line (t, tt);

        if (tt != type::eos)
          next (t, tt); // Swallow newline.

        return;
      }
    }
    catch (const invalid_argument& e) { fail (el) << e; }

    // Being here means things didn't end up well. Parse the description, if
    // any, with expansion. Then fail.
    //
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt,
                             pattern_mode::ignore,
                             "description",
                             nullptr)
              : names ());

    diag_record dr (fail (al));

    if (ns.empty ())
      dr << "assertion failed";
    else
      dr << ns;
  }

  void parser::
  parse_print (token& t, type& tt)
  {
    // Parse the rest similar to a value on the RHS of an assignment
    // (expansion, attributes).
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    if (value v = parse_value_with_attributes (t, tt, pattern_mode::expand))
    {
      names storage;
      cout << reverse (v, storage, true /* reduce */) << endl;
    }
    else
      cout << "[null]" << endl;

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  void parser::
  parse_diag (token& t, type& tt)
  {
    diag_record dr;
    const location l (get_location (t));

    switch (t.value[0])
    {
    case 'f': dr << fail (l); break;
    case 'w': dr << warn (l); break;
    case 'i': dr << info (l); break;
    case 't': dr << text (l); break;
    default: assert (false);
    }

    // Parse the rest similar to a value on the RHS of an assignment
    // (expansion, attributes).
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    if (value v = parse_value_with_attributes (t, tt, pattern_mode::expand))
    {
      names storage;
      dr << reverse (v, storage, true /* reduce */);
    }

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  void parser::
  parse_dump (token& t, type& tt)
  {
    // dump [<target>...]
    //
    // If there are no targets, then we dump the current scope.
    //
    tracer trace ("parser::parse_dump", &path_);

    const location l (get_location (t));
    next (t, tt);
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::preserve)
              : names ());

    text (l) << "dump:";

    // Dump directly into diag_stream.
    //
    ostream& os (*diag_stream);

    if (ns.empty ())
    {
      // Indent two spaces.
      //
      if (scope_ != nullptr)
        dump (scope_, nullopt /* action */, dump_format::buildfile, "  ");
      else
        os << "  <no current scope>" << endl;
    }
    else
    {
      for (auto i (ns.begin ()), e (ns.end ()); i != e; )
      {
        name& n (*i++);
        name o (n.pair ? move (*i++) : name ());

        // @@ TODO
        //
        if (n.pattern)
          fail (l) << "dumping target patterns no yet supported";

        const target* t (enter_target::find_target (*this, n, o, l, trace));

        // Indent two spaces.
        //
        if (t != nullptr)
          dump (t, nullopt /* action */, dump_format::buildfile, "  ");
        else
        {
          os << "  <no target " << n;
          if (n.pair && !o.dir.empty ()) os << '@' << o.dir;
          os << '>' << endl;
        }

        if (i != e)
          os << endl;
      }
    }

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  const variable& parser::
  parse_variable_name (string&& on, const location& l)
  {
    // Enter a variable name for assignment (as opposed to lookup).

    // If the variable is qualified (and thus public), make it overridable.
    //
    // Note that the overridability can still be restricted (e.g., by a module
    // that enters this variable or by a pattern).
    //
    bool ovr (on.find ('.') != string::npos);
    auto r (scope_->var_pool ().insert (move (on), nullptr, nullptr, &ovr));

    if (!r.second)
      return r.first;

    // If it's newly entered, verify it's not reserved for the build2 core.
    // We reserve:
    //
    // - Variable components that start with underscore (_x, x._y).
    //
    // - Variables in the `build`, `import`, and `export` namespaces.
    //
    const string& n (r.first.name);

    const char* w (
      n[0] == '_'                      ? "name starts with underscore" :
      n.find ("._") != string::npos    ? "component starts with underscore" :
      n.compare (0, 6, "build.") == 0  ? "is in 'build' namespace"  :
      n.compare (0, 7, "import.") == 0 ? "is in 'import' namespace" :
      n.compare (0, 7, "export.") == 0 ? "is in 'export' namespace" : nullptr);

    if (w != nullptr)
      fail (l) << "variable name '" << n << "' is reserved" <<
        info << "variable " << w;

    return r.first;
  }

  const variable& parser::
  parse_variable_name (names&& ns, const location& l)
  {
    // Parse and enter a variable name for assignment (as opposed to lookup).

    // The list should contain a single, simple name. Go an extra mile to
    // issue less confusing diagnostics.
    //
    size_t n (ns.size ());
    if (n == 0 || (n == 1 && ns[0].empty ()))
      fail (l) << "empty variable name";
    else if (n != 1 || ns[0].pattern || !ns[0].simple ())
      fail (l) << "expected variable name instead of " << ns;

    return parse_variable_name (move (ns[0].value), l);
  }

  void parser::
  parse_variable (token& t, type& tt, const variable& var, type kind)
  {
    // @@ TODO: yet unclear what should the logic be here: we could expect
    //    the called to handle skipping or skip it here. Need to see how
    //    everything fits.
    //
    // Note that here we treat default assignment (?=) the same as normal
    // assignment expecting the caller to check whether the assignment is
    // necessary (and skipping evaluating the value altogether otherwise).
    //
    assert (kind != type::default_assign);

    value rhs (parse_variable_value (t, tt));

    value& lhs (
      kind == type::assign

      ? (prerequisite_ != nullptr ? prerequisite_->assign (var) :
         target_ != nullptr       ? target_->assign (var)       :
         /*                      */ scope_->assign (var))

      : (prerequisite_ != nullptr ? prerequisite_->append (var, *target_) :
         target_ != nullptr       ? target_->append (var)                 :
         /*                      */ scope_->append (var)));

    apply_value_attributes (&var, lhs, move (rhs), kind);
  }

  void parser::
  parse_type_pattern_variable (
    token& t, token_type& tt,
    pattern_type pt, const target_type& ptt, string pat, const location& ploc,
    const variable& var, token_type kind, const location& loc)
  {
    // Parse target type/pattern-specific variable assignment.
    //

    // Note: expanding the value in the current scope context.
    //
    value rhs (parse_variable_value (t, tt));

    pair<reference_wrapper<value>, bool> p (rhs /* dummy */, false);
    try
    {
      // Leave the value untyped unless we are assigning.
      //
      // Note that the pattern is preserved if insert fails with regex_error.
      //
      p = scope_->target_vars[ptt].insert (pt, move (pat)).insert (
        var, kind == type::assign, false /* reset_extra */);
    }
    catch (const regex_error& e)
    {
      // Print regex_error description if meaningful (no space).
      //
      fail (ploc) << "invalid regex pattern '" << pat << "'" << e;
    }

    value& lhs (p.first);

    // We store prepend/append values untyped (similar to overrides).
    //
    if (rhs.type != nullptr && kind != type::assign)
    {
      // Our heuristics for prepend/append of a typed value is to preserve
      // empty (see apply_value_attributes() for details) so do not reduce.
      //
      untypify (rhs, false /* reduce */);
    }

    if (p.second)
    {
      // Note: we are always using assign and we don't pass the variable in
      // case of prepend/append in order to keep the value untyped.
      //
      apply_value_attributes (kind == type::assign ? &var : nullptr,
                              lhs,
                              move (rhs),
                              type::assign);

      // Map assignment type to the value::extra constant.
      //
      lhs.extra = (kind == type::prepend ? 1 :
                   kind == type::append  ? 2 :
                   0);
    }
    else
    {
      // Existing value. What happens next depends on what we are trying to do
      // and what's already there.
      //
      // Assignment is the easy one: we simply overwrite what's already
      // there. Also, if we are appending/prepending to a previously assigned
      // value, then we simply append or prepend normally.
      //
      if (kind == type::assign || lhs.extra == 0)
      {
        // Above we've instructed insert() not to type the value so we have to
        // compensate for that now.
        //
        if (kind != type::assign)
        {
          if (var.type != nullptr && lhs.type != var.type)
            typify (lhs, *var.type, &var);
        }
        else
          lhs.extra = 0; // Change to assignment.

        apply_value_attributes (&var, lhs, move (rhs), kind);
      }
      else
      {
        // This is an append/prepent to a previously appended or prepended
        // value. We can handle it as long as things are consistent.
        //
        if (kind == type::prepend && lhs.extra == 2)
          fail (loc) << "prepend to a previously appended target type/pattern-"
                     << "specific variable " << var;

        if (kind == type::append && lhs.extra == 1)
          fail (loc) << "append to a previously prepended target type/pattern-"
                     << "specific variable " << var;

        // Do untyped prepend/append.
        //
        apply_value_attributes (nullptr, lhs, move (rhs), kind);
      }
    }

    if (lhs.extra != 0 && lhs.type != nullptr)
      fail (loc) << "typed prepend/append to target type/pattern-specific "
                 << "variable " << var;
  }

  value parser::
  parse_variable_value (token& t, type& tt, bool m)
  {
    if (m)
    {
      mode (lexer_mode::value, '@');
      next_with_attributes (t, tt);
    }
    else
      next (t, tt);

    // Parse value attributes if any. Note that it's ok not to have anything
    // after the attributes (e.g., foo=[null]).
    //
    attributes_push (t, tt, true);

    return tt != type::newline && tt != type::eos
      ? parse_value (t, tt, pattern_mode::expand)
      : value (names ());
  }

  const value_type* parser::
  find_value_type (const scope*, const string& n)
  {
    switch (n[0])
    {
    case 'a':
      {
        if (n == "abs_dir_path") return &value_traits<abs_dir_path>::value_type;
        break;
      }
    case 'b':
      {
        if (n == "bool") return &value_traits<bool>::value_type;
        break;
      }
    case 'c':
      {
        if (n == "cmdline") return &value_traits<cmdline>::value_type;
        break;
      }
    case 'd':
      {
        if (n.compare (0, 8, "dir_path") == 0)
        {
          if (n[8] == '\0') return &value_traits<dir_path>::value_type;
          if (n[8] == 's' &&
              n[9] == '\0') return &value_traits<dir_paths>::value_type;
        }
        break;
      }
    case 'i':
      {
        if (n.compare (0, 5, "int64") == 0)
        {
          if (n[5] == '\0') return &value_traits<int64_t>::value_type;
          if (n[5] == 's' &&
              n[6] == '\0') return &value_traits<int64s>::value_type;
        }
        break;
      }
    case 'j':
      {
        if (n.compare (0, 4, "json") == 0)
        {
          if (n[4] == '\0') return &value_traits<json_value>::value_type;
          if (n == "json_array") return &value_traits<json_array>::value_type;
          if (n == "json_object")
            return &value_traits<json_object>::value_type;
          if (n == "json_set")
            return &value_traits<set<json_value>>::value_type;
          if (n == "json_map")
            return &value_traits<map<json_value, json_value>>::value_type;
        }
        break;
      }
    case 'n':
      {
        if (n.compare (0, 4, "name") == 0)
        {
          if (n[4] == '\0') return &value_traits<name>::value_type;
          if (n[4] == 's' &&
              n[5] == '\0') return &value_traits<vector<name>>::value_type;
          if (n == "name_pair") return &value_traits<name_pair>::value_type;
        }
        break;
      }

    case 'p':
      {
        if (n.compare (0, 4, "path") == 0)
        {
          if (n[4] == '\0') return &value_traits<path>::value_type;
          if (n[4] == 's' &&
              n[5] == '\0') return &value_traits<paths>::value_type;
        }
        else if (n == "project_name")
          return &value_traits<project_name>::value_type;
        break;
      }
    case 's':
      {
        if (n.compare (0, 6, "string") == 0)
        {
          if (n[6] == '\0') return &value_traits<string>::value_type;
          if (n[6] == 's' &&
              n[7] == '\0') return &value_traits<strings>::value_type;
          if (n == "string_set") return &value_traits<set<string>>::value_type;
          if (n == "string_map")
            return &value_traits<map<string,string>>::value_type;
        }
        break;
      }
    case 't':
      {
        if (n == "target_triplet")
          return &value_traits<target_triplet>::value_type;
        break;
      }
    case 'u':
      {
        if (n.compare (0, 6, "uint64") == 0)
        {
          if (n[6] == '\0') return &value_traits<uint64_t>::value_type;
          if (n[6] == 's' &&
              n[7] == '\0') return &value_traits<uint64s>::value_type;
        }
        break;
      }
    default:
      break;
    }

    return nullptr;
  }

  void parser::
  apply_variable_attributes (const variable& var)
  {
    attributes as (attributes_pop ());

    if (as.empty ())
      return;

    const location& l (as.loc);

    const value_type* type (nullptr);
    optional<variable_visibility> vis;
    optional<bool> ovr;

    for (auto& a: as)
    {
      string& n (a.name);
      value& v (a.value);

      if (n == "visibility")
      {
        try
        {
          string s (convert<string> (move (v)));

          variable_visibility r;
          if      (s == "global")       r = variable_visibility::global;
          else if (s == "project")      r = variable_visibility::project;
          else if (s == "scope")        r = variable_visibility::scope;
          else if (s == "target")       r = variable_visibility::target;
          else if (s == "prerequisite") r = variable_visibility::prereq;
          else throw invalid_argument ("unknown visibility name");

          if (vis && r != *vis)
            fail (l) << "conflicting variable visibilities: " << s << ", "
                     << *vis;

          vis = r;
        }
        catch (const invalid_argument& e)
        {
          fail (l) << "invalid " << n << " attribute value: " << e;
        }
      }
      else if (n == "overridable")
      {
        try
        {
          // Treat absent value (represented as NULL) as true.
          //
          bool r (v.null || convert<bool> (move (v)));

          if (ovr && r != *ovr)
            fail (l) << "conflicting variable overridabilities";

          ovr = r;
        }
        catch (const invalid_argument& e)
        {
          fail (l) << "invalid " << n << " attribute value: " << e;
        }
      }
      else if (const value_type* t = find_value_type (root_, n))
      {
        if (!v.null)
          fail (l) << "unexpected value in attribute " << a;

        if (type != nullptr && t != type)
          fail (l) << "conflicting variable types: " << n << ", "
                   << type->name;

        type = t;
      }
      else
        fail (l) << "unknown variable attribute " << a;
    }

    if (type != nullptr && var.type != nullptr)
    {
      if (var.type == type)
        type = nullptr;
      else
        fail (l) << "changing variable " << var << " type from "
                 << var.type->name << " to " << type->name;
    }

    if (vis)
    {
      // Note that this logic naturally makes sure that a project-private
      // variable doesn't have global visibility (since it would have been
      // entered with the project visibility).
      //
      if (var.visibility == *vis)
        vis = nullopt;
      else if (var.visibility > *vis) // See variable_pool::update().
        fail (l) << "changing variable " << var << " visibility from "
                 << var.visibility << " to " << *vis;
    }

    if (ovr)
    {
      // Note that the overridability incompatibilities are diagnosed by
      // update(). So we just need to diagnose the project-private case.
      //
      if (*ovr && var.owner != &ctx->var_pool)
        fail (l) << "private variable " << var << " cannot be overridable";
    }

    if (type || vis || ovr)
      var.owner->update (const_cast<variable&> (var),
                         type,
                         vis ? &*vis : nullptr,
                         ovr ? &*ovr : nullptr);
  }

  void parser::
  apply_value_attributes (const variable* var,
                          value& v,
                          value&& rhs,
                          type kind)
  {
    attributes as (attributes_pop ());
    const location& l (as.loc); // This points to value if no attributes.

    // Essentially this is an attribute-augmented assign/append/prepend.
    //
    bool null (false);
    const value_type* type (nullptr);

    for (auto& a: as)
    {
      string& n (a.name);
      value& v (a.value);

      if (n == "null")
      {
        // @@ Looks like here we assume representationally empty?
        //
        if (rhs && !rhs.empty ()) // Note: null means we had an expansion.
          fail (l) << "value with null attribute";

        null = true;
        // Fall through.
      }
      else if (const value_type* t = find_value_type (root_, n))
      {
        if (type != nullptr && t != type)
          fail (l) << "conflicting value types: " << n << ", " << type->name;

        type = t;
        // Fall through.
      }
      else
        fail (l) << "unknown value attribute " << a;

      if (!v.null)
        fail (l) << "unexpected value in attribute " << a;
    }

    // When do we set the type and when do we keep the original? This gets
    // tricky for append/prepend where both values contribute. The guiding
    // rule here is that if the user specified the type, then they reasonable
    // expect the resulting value to be of that type. So for assign we always
    // override the type since it's a new value. For append/prepend we
    // override if the LHS value is NULL (which also covers undefined). We
    // also override if LHS is untyped. Otherwise, we require that the types
    // be the same. Also check that the requested value type doesn't conflict
    // with the variable type.
    //
    if (var != nullptr && var->type != nullptr)
    {
      if (type == nullptr)
      {
        type = var->type;
      }
      else if (var->type != type)
      {
        fail (l) << "conflicting variable " << var->name << " type "
                 << var->type->name << " and value type " << type->name;
      }
    }

    // What if both LHS and RHS are typed? For now we do lexical conversion:
    // if this specific value can be converted, then all is good. The
    // alternative would be to do type conversion: if any value of RHS type
    // can be converted to LHS type, then we are good. This may be a better
    // option in the future but currently our parse_names() implementation
    // untypifies everything if there are multiple names. And having stricter
    // rules just for single-element values would be strange.
    //
    // We also have "weaker" type propagation for the RHS type.
    //
    bool rhs_type (false);
    if (rhs.type != nullptr)
    {
      // Our heuristics is to not reduce typed RHS empty simple values for
      // prepend/append and additionally for assign provided LHS is a
      // container.
      //
      bool reduce (kind == type::assign &&
                   (type == nullptr || !type->container));

      // Only consider RHS type if there is no explicit or variable type.
      //
      if (type == nullptr)
      {
        type = rhs.type;
        rhs_type = true;
      }

      // Reduce this to the untyped value case for simplicity.
      //
      untypify (rhs, reduce);
    }

    if (kind == type::assign)
    {
      if (type != v.type)
      {
        v = nullptr; // Clear old value.
        v.type = type;
      }
    }
    else if (type != nullptr)
    {
      if (!v)
        v.type = type;
      else if (v.type == nullptr)
        typify (v, *type, var);
      else if (v.type != type && !rhs_type)
        fail (l) << "conflicting original value type " << v.type->name
                 << " and append/prepend value type " << type->name;
    }

    if (null)
    {
      if (kind == type::assign) // Ignore for prepend/append.
        v = nullptr;
    }
    else
    {
      auto df = make_diag_frame (
        [this, var, &l](const diag_record& dr)
        {
          if (!l.empty ())
          {
            dr << info (l);
            if (var != nullptr) dr << "variable " << var->name << ' ';
            dr << "value is assigned here";
          }
        });

      if (kind == type::assign)
      {
        if (rhs)
          v.assign (move (rhs).as<names> (), var);
        else
          v = nullptr;
      }
      else if (rhs) // Don't append/prepent NULL.
      {
        if (kind == type::prepend)
          v.prepend (move (rhs).as<names> (), var);
        else
          v.append (move (rhs).as<names> (), var);
      }
    }
  }

  value parser::
  parse_value_with_attributes (token& t, token_type& tt,
                               pattern_mode pmode,
                               const char* what,
                               const string* separators,
                               bool chunk)
  {
    // Parse value attributes if any. Note that it's ok not to have anything
    // after the attributes (think [null]).
    //
    attributes_push (t, tt, true);

    value rhs (tt != type::newline && tt != type::eos
               ? parse_value (t, tt, pmode, what, separators, chunk)
               : value (names ()));

    if (pre_parse_)
      return rhs; // Empty.

    value lhs;
    apply_value_attributes (nullptr, lhs, move (rhs), type::assign);
    return lhs;
  }

  values parser::
  parse_eval (token& t, type& tt, pattern_mode pmode)
  {
    // enter: token after lparen (lexed in the eval mode with attributes).
    // leave: rparen             (eval mode auto-expires at rparen).

    if (tt == type::rparen)
      return values ();

    values r (parse_eval_comma (t, tt, pmode, true));

    if (tt == type::backtick) // @@ TMP
      fail (t) << "arithmetic evaluation context not yet supported";

    if (tt == type::bit_or) // @@ TMP
      fail (t) << "evaluation pipeline not yet supported";

    if (tt != type::rparen)
      fail (t) << "unexpected " << t; // E.g., stray ':'.

    return r;
  }

  values parser::
  parse_eval_comma (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    values r;
    value lhs (parse_eval_ternary (t, tt, pmode, first));

    if (!pre_parse_)
      r.push_back (move (lhs));

    while (tt == type::comma)
    {
      next_with_attributes (t, tt); // Recognize attributes before value.

      value rhs (parse_eval_ternary (t, tt, pmode));

      if (!pre_parse_)
        r.push_back (move (rhs));
    }

    return r;
  }

  value parser::
  parse_eval_ternary (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Right-associative (kind of): we parse what's between ?: without
    // regard for priority and we recurse on what's after :. Here is an
    // example:
    //
    // a ? x ? y : z : b ? c : d
    //
    // This should be parsed/evaluated as:
    //
    // a ? (x ? y : z) : (b ? c : d)
    //
    location l (get_location (t));
    value lhs (parse_eval_or (t, tt, pmode, first));

    if (tt != type::question)
      return lhs;

    location ql (get_location (t));

    // Use the pre-parse mechanism to implement short-circuit.
    //
    bool pp (pre_parse_);

    bool q;
    try
    {
      q = pp ? true : convert<bool> (move (lhs));
    }
    catch (const invalid_argument& e)
    {
      fail (l)    << e <<
        info (ql) << "use the '\\?' escape sequence if this is a wildcard "
                  << "pattern" << endf;
    }

    if (!pp)
      pre_parse_ = !q; // Short-circuit middle?

    next_with_attributes (t, tt); // Recognize attributes before value.

    value mhs (parse_eval_ternary (t, tt, pmode));

    if (tt != type::colon)
    {
      fail (t)    << "expected ':' instead of " << t <<
        info (ql) << "use the '\\?' escape sequence if this is a wildcard "
                  << "pattern" << endf;
    }

    if (!pp)
      pre_parse_ = q; // Short-circuit right?

    next_with_attributes (t, tt); // Recognize attributes before value.

    value rhs (parse_eval_ternary (t, tt, pmode));

    pre_parse_ = pp;
    return q ? move (mhs) : move (rhs);
  }

  value parser::
  parse_eval_or (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    location l (get_location (t));
    value lhs (parse_eval_and (t, tt, pmode, first));

    // Use the pre-parse mechanism to implement short-circuit.
    //
    bool pp (pre_parse_);

    while (tt == type::log_or)
    {
      try
      {
        if (!pre_parse_ && convert<bool> (move (lhs)))
          pre_parse_ = true;

        next_with_attributes (t, tt); // Recognize attributes before value.

        l = get_location (t);
        value rhs (parse_eval_and (t, tt, pmode));

        if (pre_parse_)
          continue;

        // Store the result as bool value.
        //
        lhs = convert<bool> (move (rhs));
      }
      catch (const invalid_argument& e) { fail (l) << e; }
    }

    pre_parse_ = pp;
    return lhs;
  }

  value parser::
  parse_eval_and (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    location l (get_location (t));
    value lhs (parse_eval_comp (t, tt, pmode, first));

    // Use the pre-parse mechanism to implement short-circuit.
    //
    bool pp (pre_parse_);

    while (tt == type::log_and)
    {
      try
      {
        if (!pre_parse_ && !convert<bool> (move (lhs)))
          pre_parse_ = true;

        next_with_attributes (t, tt); // Recognize attributes before value.

        l = get_location (t);
        value rhs (parse_eval_comp (t, tt, pmode));

        if (pre_parse_)
          continue;

        // Store the result as bool value.
        //
        lhs = convert<bool> (move (rhs));
      }
      catch (const invalid_argument& e) { fail (l) << e; }
    }

    pre_parse_ = pp;
    return lhs;
  }

  value parser::
  parse_eval_comp (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    value lhs (parse_eval_value (t, tt, pmode, first));

    while (tt == type::equal      ||
           tt == type::not_equal  ||
           tt == type::less       ||
           tt == type::less_equal ||
           tt == type::greater    ||
           tt == type::greater_equal)
    {
      type op (tt);
      location l (get_location (t));

      next_with_attributes (t, tt); // Recognize attributes before value.

      value rhs (parse_eval_value (t, tt, pmode));

      if (pre_parse_)
        continue;

      // Store the result as a bool value.
      //
      lhs = value (compare_values (op, lhs, rhs, l));
    }

    return lhs;
  }

  value parser::
  parse_eval_value (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of value (lexed with enabled attributes)
    // leave: next token after value

    // Parse value attributes if any. Note that it's ok not to have anything
    // after the attributes, as in, ($foo == [null]), or even ([null])
    //
    auto at (attributes_push (t, tt, true));

    const location l (get_location (t));

    value v;
    switch (tt)
    {
    case type::log_not:
      {
        next_with_attributes (t, tt); // Recognize attributes before value.

        v = parse_eval_value (t, tt, pmode);

        if (pre_parse_)
          break;

        try
        {
          // Store the result as bool value.
          //
          v = !convert<bool> (move (v));
        }
        catch (const invalid_argument& e) { fail (l) << e; }
        break;
      }
    default:
      {
        // If parse_value() gets called, it expects to see a value. Note that
        // it will also handle nested eval contexts.
        //
        v = (tt != type::colon         &&
             tt != type::question      &&
             tt != type::comma         &&

             tt != type::rparen        &&

             tt != type::equal         &&
             tt != type::not_equal     &&
             tt != type::less          &&
             tt != type::less_equal    &&
             tt != type::greater       &&
             tt != type::greater_equal &&

             tt != type::log_or        &&
             tt != type::log_and

             ? parse_value (t, tt, pmode)
             : value (names ()));
      }
    }

    // If this is the first expression then handle the eval-qual special case
    // (target-qualified name represented as a special ':'-style pair).
    //
    if (first && tt == type::colon)
    {
      if (at.first)
        fail (at.second) << "attributes before target-qualified variable name";

      if (!pre_parse_)
        attributes_pop ();

      const location nl (get_location (t));
      next (t, tt);
      value n (parse_value (t, tt, pattern_mode::preserve));

      if (tt != type::rparen)
        fail (t) << "expected ')' after variable name";

      if (pre_parse_)
        return v; // Empty.

      // We used to return this as a <target>:<name> pair but that meant we
      // could not handle an out-qualified target (which is represented as
      // <target>@<out> pair). As a somewhat of a hack, we deal with this by
      // changing the order of the name and target to be <name>:<target> with
      // the qualified case becoming a "tripple pair" <name>:<target>@<out>.
      //
      // @@ This is actually not great since it's possible to observe such a
      //    tripple pair, for example with `print (file{x}@./:y)`.
      //
      if (n.type != nullptr || !n || n.as<names> ().size () != 1 ||
          n.as<names> ()[0].pattern)
        fail (nl) << "expected variable name after ':'";

      names& ns (n.as<names> ());
      ns.back ().pair = ':';

      if (v.type == nullptr && v)
      {
        names& ts (v.as<names> ());

        size_t s (ts.size ());
        if (s == 1 || (s == 2 && ts.front ().pair == '@'))
        {
          ns.push_back (move (ts.front ()));
          if (s == 2)
            ns.push_back (move (ts.back ()));

          return n;
        }
      }

      fail (l) << "expected target before ':'" << endf;
    }
    else
    {
      if (pre_parse_)
        return v; // Empty.

      // Process attributes if any.
      //
      if (attributes_top ().empty ())
      {
        attributes_pop ();
        return v;
      }

      value r;
      apply_value_attributes (nullptr, r, move (v), type::assign);
      return r;
    }
  }

  bool parser::
  compare_values (type op, value& lhs, value& rhs, const location& loc) const
  {
    // Use (potentially typed) comparison via value. If one of the values is
    // typed while the other is not, then try to convert the untyped one to
    // the other's type instead of complaining. This seems like a reasonable
    // thing to do and will allow us to write:
    //
    // if ($build.version > 30000)
    //
    // Rather than having to write:
    //
    // if ($build.version > [uint64] 30000)
    //
    if (lhs.type != rhs.type)
    {
      // @@ Would be nice to pass location for diagnostics.
      //
      if (lhs.type == nullptr)
      {
        if (lhs)
          typify (lhs, *rhs.type, nullptr);
      }
      else if (rhs.type == nullptr)
      {
        if (rhs)
          typify (rhs, *lhs.type, nullptr);
      }
      else
        fail (loc) << "comparison between " << lhs.type->name << " and "
                   << rhs.type->name;
    }

    bool r;
    switch (op)
    {
    case type::equal:         r = lhs == rhs; break;
    case type::not_equal:     r = lhs != rhs; break;
    case type::less:          r = lhs <  rhs; break;
    case type::less_equal:    r = lhs <= rhs; break;
    case type::greater:       r = lhs >  rhs; break;
    case type::greater_equal: r = lhs >= rhs; break;
    default:                  r = false;      assert (false);
    }
    return r;
  }

  pair<bool, location> parser::
  attributes_push (token& t, type& tt, bool standalone, bool next_token)
  {
    // To make sure that the attributes are not standalone we need to read the
    // token which follows ']'.
    //
    assert (standalone || next_token);

    location l (get_location (t));
    bool has (tt == type::lsbrace);

    if (!pre_parse_)
      attributes_.push_back (attributes (l));

    if (!has)
      return make_pair (false, l);

    mode (lexer_mode::attributes);
    next (t, tt);

    if (tt != type::rsbrace)
    {
      do
      {
        if (tt == type::newline || tt == type::eos)
          break;

        // Parse the attribute name with expansion (we rely on this in some
        // old and hairy tests).
        //
        // Note that the attributes lexer mode does not recognize `{}@` as
        // special and we rely on that in the rule hint attributes
        // (libs@rule_hint=cxx).
        //
        const location l (get_location (t));

        names ns (
          parse_names (t, tt, pattern_mode::ignore, "attribute", nullptr));

        string n;
        value v;

        if (!pre_parse_)
        {
          // The list should contain a single, simple name.
          //
          if (ns.size () != 1 || !ns[0].simple () || ns[0].empty ())
            fail (l) << "expected attribute name instead of " << ns;

          n = move (ns[0].value);
        }

        if (tt == type::assign)
        {
          // To handle the value we switch into the attribute_value mode
          // (which doesn't treat `=` as special).
          //
          mode (lexer_mode::attribute_value, '@');
          next (t, tt);

          v = (tt != type::comma && tt != type::rsbrace
               ? parse_value (t, tt, pattern_mode::ignore, "attribute value")
               : value (names ()));

          expire_mode ();
        }

        if (!pre_parse_)
          attributes_.back ().push_back (attribute {move (n), move (v)});

        if (tt == type::comma)
          next (t, tt);
      }
      while (tt != type::rsbrace);
    }
    else
      has = false; // `[]` doesn't count.

    if (tt != type::rsbrace)
      fail (t) << "expected ']' instead of " << t;

    if (next_token)
    {
      next (t, tt);

      if (tt == type::newline || tt == type::eos)
      {
        if (!standalone)
          fail (t) << "standalone attributes";
      }
      //
      // Verify that the attributes are separated from the following word or
      // "word-producing" token.
      //
      else if (!t.separated && (tt == type::word   ||
                                tt == type::dollar ||
                                tt == type::lparen ||
                                tt == type::lcbrace))
        fail (t)   << "whitespace required after attributes" <<
          info (l) << "use the '\\[' escape sequence if this is a wildcard "
                   << "pattern";
    }

    return make_pair (has, l);
  }

  // Add a name verifying it is valid.
  //
  static inline name&
  append_name (names& ns,
               optional<project_name> p,
               dir_path d,
               string t,
               string v,
               optional<name::pattern_type> pat,
               const location& loc)
  {
    // The directory/value must not be empty if we have a type.
    //
    if (d.empty () && v.empty () && !t.empty ())
      fail (loc) << "typed empty name";

    ns.emplace_back (move (p), move (d), move (t), move (v), pat);
    return ns.back ();
  }

  // Splice names from the name view into the destination name list while
  // doing sensible things with pairs, types, etc. Return the number of
  // the names added.
  //
  // If nv points to nv_storage then the names can be moved.
  //
  size_t parser::
  splice_names (const location& loc,
                const names_view& nv,
                names&& nv_storage,
                names& ns,
                const char* what,
                size_t pairn,
                const optional<project_name>& pp,
                const dir_path* dp,
                const string* tp)
  {
    // We could be asked to splice 0 elements (see the name pattern
    // expansion). In this case may need to pop the first half of the
    // pair.
    //
    if (nv.size () == 0)
    {
      if (pairn != 0)
        ns.pop_back ();

      return 0;
    }

    size_t start (ns.size ());

    // Move if nv points to nv_storage,
    //
    bool m (nv.data () == nv_storage.data ());

    for (const name& cn: nv)
    {
      name* n (m ? const_cast<name*> (&cn) : nullptr);

      // Project.
      //
      optional<project_name> p;
      if (cn.proj)
      {
        if (pp)
          fail (loc) << "nested project name " << *cn.proj << " in " << what;

        p = m ? move (n->proj) : cn.proj;
      }
      else if (pp)
        p = pp;

      // Directory.
      //
      dir_path d;
      if (!cn.dir.empty ())
      {
        if (dp != nullptr)
        {
          if (cn.dir.absolute ())
            fail (loc) << "nested absolute directory " << cn.dir << " in "
                       << what;

          d = *dp / cn.dir;
        }
        else
          d = m ? move (n->dir) : cn.dir;
      }
      else if (dp != nullptr)
        d = *dp;

      // Type.
      //
      string t;
      if (!cn.type.empty ())
      {
        if (tp != nullptr)
          fail (loc) << "nested type name " << cn.type << " in " << what;

        t = m ? move (n->type) : cn.type;
      }
      else if (tp != nullptr)
        t = *tp;

      // Value.
      //
      string v (m ? move (n->value) : cn.value);

      // If we are a second half of a pair.
      //
      if (pairn != 0)
      {
        // Check that there are no nested pairs.
        //
        if (cn.pair)
          fail (loc) << "nested pair in " << what;

        // And add another first half unless this is the first instance.
        //
        if (pairn != ns.size ())
          ns.push_back (ns[pairn - 1]);
      }

      name& r (
        append_name (ns,
                     move (p), move (d), move (t), move (v), cn.pattern,
                     loc));
      r.pair = cn.pair;
    }

    return ns.size () - start;
  }

  // Expand a name pattern. Note that the result can be empty (as in "no
  // elements").
  //
  size_t parser::
  expand_name_pattern (const location& l,
                       names&& pat,
                       names& ns,
                       const char* what,
                       size_t pairn,
                       const dir_path* dp,
                       const string* tp,
                       const target_type* tt)
  {
    assert (!pat.empty () && (tp == nullptr || tt != nullptr));

    // We are going to accumulate the result in a vector which can result in
    // quite a few linear searches. However, thanks to a few optimizations,
    // this shouldn't be an issue for the common cases (e.g., a pattern plus
    // a few exclusions).
    //
    names r;
    bool dir (false);

    // Figure out the start directory.
    //
    const dir_path* sp;
    dir_path s;
    if (dp != nullptr)
    {
      if (dp->absolute ())
        sp = dp;
      else
      {
        s = *pbase_ / *dp;
        sp = &s;
      }
    }
    else
      sp = pbase_;

    // Compare string to name as paths and according to dir.
    //
    auto equal = [&dir] (const string& v, const name& n) -> bool
    {
      // Use path comparison (which may be slash/case-insensitive).
      //
      return path::traits_type::compare (
        v, dir ? n.dir.representation () : n.value) == 0;
    };

    // Compare name to pattern as paths and according to dir.
    //
    auto match = [&dir, sp] (const name& n, const path& pattern) -> bool
    {
      const path& p (dir ? path_cast<path> (n.dir) : path (n.value));
      return path_match (p, pattern, *sp);
    };

    // Append name/extension to result according to dir. Store an indication
    // of whether it was amended as well as whether the extension is present
    // in the pair flag. The extension itself is stored in name::type.
    //
    auto append = [&r, &dir] (string&& v, optional<string>&& e, bool a)
    {
      // Here we can assume either dir or value are not empty (comes from
      // pattern expansion).
      //
      name n (dir ? name (dir_path (move (v))) : name (move (v)));

      if (a)
        n.pair |= 0x01;

      if (e)
      {
        n.type = move (*e);
        n.pair |= 0x02;
      }

      r.push_back (move (n));
    };

    auto include_match = [&r, &equal, &append] (string&& m,
                                                optional<string>&& e,
                                                bool a)
    {
      auto i (find_if (
                r.begin (),
                r.end (),
                [&m, &equal] (const name& n) {return equal (m, n);}));

      if (i == r.end ())
        append (move (m), move (e), a);
    };

    // May throw invalid_path.
    //
    auto include_pattern =
      [this,
       &append, &include_match,
       &r, sp, &l, &dir] (string&& p,
                          optional<string>&& e,
                          bool a)
    {
      // If we don't already have any matches and our pattern doesn't contain
      // multiple recursive wildcards, then the result will be unique and we
      // can skip checking for duplicated. This should help quite a bit in the
      // common cases where we have a pattern plus maybe a few exclusions.
      //
      bool unique (r.empty () && path_pattern_recursive (path (p)) <= 1);

      struct data
      {
        const optional<string>& e;
        const dir_path& sp;
        function<void (string&&, optional<string>&&)> appf;

      } d {e, *sp, nullptr};

      if (unique)
        d.appf = [a, &append] (string&& v, optional<string>&& e)
        {
          append (move (v), move (e), a);
        };
      else
        d.appf = [a, &include_match] (string&& v, optional<string>&& e)
        {
          include_match (move (v), move (e), a);
        };

      auto process = [&d, this] (path&& m, const string& p, bool interm)
      {
        // Ignore entries that start with a dot unless the pattern that
        // matched them also starts with a dot. Also ignore directories
        // containing the .buildignore file (ignoring the test if we don't
        // have a sufficiently setup project root).
        //
        const string& s (m.string ());
        if ((p[0] != '.' && s[path::traits_type::find_leaf (s)] == '.') ||
            (root_ != nullptr              &&
             root_->root_extra != nullptr  &&
             m.to_directory ()             &&
             exists (d.sp / m / root_->root_extra->buildignore_file)))
          return !interm;

        // Note that we have to make copies of the extension since there will
        // multiple entries for each pattern.
        //
        if (!interm)
        {
          // If the extension is empty (meaning there should be no extension,
          // for example hxx{Q*.}), skip entries with extensions.
          //
          if (!d.e || !d.e->empty () || m.extension_cstring () == nullptr)
            d.appf (move (m).representation (), optional<string> (d.e));
        }

        return true;
      };

      const function<bool (const dir_entry&)> dangling (
        [&dir] (const dir_entry& de)
        {
          bool sl (de.ltype () == entry_type::symlink);

          const path& n (de.path ());

          // One case where this turned out to be not worth it practically
          // (too much noise) is the backlinks to executables (and the
          // associated DLL assemblies for Windows). So we now have this
          // heuristics that if this looks like an executable (or DLL for
          // Windows), then we omit the warning. On POSIX, where executables
          // don't have extensions, we will consider it an executable only if
          // we are not looking for directories (which also normally don't
          // have extension).
          //
          // @@ PEDANTIC: re-enable if --pedantic.
          //
          if (sl)
          {
            string e (n.extension ());

            if ((e.empty () && !dir)                 ||
                path_traits::compare (e, "exe") == 0 ||
                path_traits::compare (e, "dll") == 0 ||
                path_traits::compare (e, "pdb") == 0 ||   // .{exe,dll}.pdb
                (path_traits::compare (e, "dlls") == 0 && // .exe.dlls assembly
                 path_traits::compare (n.base ().extension (), "exe") == 0))
              return true;
          }

          warn << "skipping "
               << (sl ? "dangling symlink" : "inaccessible entry")
               << ' ' << de.base () / n;

          return true;
        });

      try
      {
        path_search (path (move (p)),
                     process,
                     *sp,
                     path_match_flags::follow_symlinks,
                     dangling);
      }
      catch (const system_error& e)
      {
        fail (l) << "unable to scan " << *sp << ": " << e;
      }
    };

    auto exclude_match = [&r, &equal] (const string& m)
    {
      // We know there can only be one element so we use find_if() instead of
      // remove_if() for efficiency.
      //
      auto i (find_if (
                r.begin (),
                r.end (),
                [&m, &equal] (const name& n) {return equal (m, n);}));

      if (i != r.end ())
        r.erase (i);
    };

    auto exclude_pattern = [&r, &match] (const path& p)
    {
      for (auto i (r.begin ()); i != r.end (); )
      {
        if (match (*i, p))
          i = r.erase (i);
        else
          ++i;
      }
    };

    // Process the pattern and inclusions/exclusions.
    //
    for (auto b (pat.begin ()), i (b), end (pat.end ()); i != end; ++i)
    {
      name& n (*i);
      bool first (i == b);

      char s ('\0'); // Inclusion/exclusion sign (+/-).

      // Reduce inclusions/exclusions group (-/+{foo bar}) to simple name/dir.
      //
      if (n.typed () && n.type.size () == 1)
      {
        if (!first)
        {
          s = n.type[0];

          if (s == '-' || s == '+')
            n.type.clear ();
        }
        else
        {
          assert (n.type[0] == '+'); // Can only belong to inclusion group.
          n.type.clear ();
        }
      }

      if (n.empty () || !(n.simple () || n.directory ()))
        fail (l) << "invalid '" << n << "' in " << what << " pattern";

      string v (n.simple () ? move (n.value) : move (n.dir).representation ());

      // Figure out if this is inclusion or exclusion.
      //
      if (first)
        s = '+'; // Treat as inclusion.
      else if (s == '\0')
      {
        s = v[0];

        assert (s == '-' || s == '+'); // Validated at the token level.
        v.erase (0, 1);

        if (v.empty ())
          fail (l) << "empty " << what << " pattern";
      }

      // Amend the pattern or match in a target type-specific manner.
      //
      // Name splitting must be consistent with scope::find_target_type().
      // Since we don't do it for directories, we have to delegate it to the
      // target_type::pattern() call.
      //
      bool a (false);     // Amended.
      optional<string> e; // Extension.
      {
        bool d;

        if (tt != nullptr && tt->pattern != nullptr)
        {
          a = tt->pattern (*tt, *scope_, v, e, l, false);
          d = path::traits_type::is_separator (v.back ());
        }
        else
        {
          d = path::traits_type::is_separator (v.back ());

          if (!d)
            e = target::split_name (v, l);
        }

        // Based on the first pattern verify inclusions/exclusions are
        // consistently file/directory.
        //
        if (first)
          dir = d;
        else if (d != dir)
          fail (l) << "inconsistent file/directory result in " << what
                   << " pattern";
      }

      // Factor non-empty extension back into the name for searching.
      //
      // Note that doing it at this stage means we don't support extension
      // patterns.
      //
      if (e && !e->empty ())
      {
        v += '.';
        v += *e;

        if (path_pattern (*e))
          fail (l) << "extension pattern in '" << v << "' (" << what
                   << " extension patterns are not yet supported)";
      }

      try
      {
        if (s == '+')
          include_pattern (move (v), move (e), a);
        else
        {
          path p (move (v));

          if (path_pattern (p))
            exclude_pattern (p);
          else
            exclude_match (move (p).representation ()); // Reuse the buffer.
        }
      }
      catch (const invalid_path& e)
      {
        fail (l) << "invalid path '" << e.path << "' in " << what
                 << " pattern";
      }
    }

    // Post-process the result: remove extension, reverse target type-specific
    // pattern/match amendments (essentially: cxx{*} -> *.cxx -> foo.cxx ->
    // cxx{foo}), and recombine the result.
    //
    for (name& n: r)
    {
      string v;
      optional<string> e;

      if (dir)
        v = move (n.dir).representation ();
      else
      {
        v = move (n.value);

        if ((n.pair & 0x02) != 0)
        {
          e = move (n.type);
          n.type.clear ();

          // Remove non-empty extension from the name (it got to be there, see
          // above).
          //
          if (!e->empty ())
            v.resize (v.size () - e->size () - 1);
        }
      }

      bool de (false); // Default extension.
      if ((n.pair & 0x01) != 0)
      {
        de = static_cast<bool> (e);
        tt->pattern (*tt, *scope_, v, e, l, true);
        de = de && !e;
      }

      if (dir)
        n.dir = dir_path (move (v));
      else
      {
        target::combine_name (v, e, de);
        n.value = move (v);
      }

      n.pair = '\0';
    }

    return splice_names (
      l, names_view (r), move (r), ns, what, pairn, nullopt, dp, tp);
  }

  // Parse names inside {} and handle the following "crosses" (i.e.,
  // {a b}{x y}) if any. Return the number of names added to the list.
  //
  size_t parser::
  parse_names_trailer (token& t, type& tt,
                       names& ns,
                       pattern_mode pmode,
                       const char* what,
                       const string* separators,
                       size_t pairn,
                       const optional<project_name>& pp,
                       const dir_path* dp,
                       const string* tp,
                       bool cross)
  {
    if (pp)
      pmode = pattern_mode::preserve;

    next (t, tt);                          // Get what's after '{'.
    const location loc (get_location (t)); // Start of names.

    size_t start (ns.size ());

    if (pairn == 0 && start != 0 && ns.back ().pair)
      pairn = start;

    names r;

    // Parse names until closing '}' expanding patterns.
    //
    auto parse = [&r, &t, &tt, pmode, what, separators, this] (
      const optional<project_name>& pp,
      const dir_path* dp,
      const string* tp)
    {
      const location loc (get_location (t));

      size_t start (r.size ());

      // This can be an ordinary name group or a pattern (with inclusions and
      // exclusions). We want to detect which one it is since for patterns we
      // want just the list of simple names without pair/dir/type added (those
      // are added after the pattern expansion in expand_name_pattern()).
      //
      // Detecting which one it is is tricky. We cannot just peek at the token
      // and look for some wildcards since the pattern can be the result of an
      // expansion (or, worse, concatenation). Thus pattern_mode::detect: we
      // are going to ask parse_names() to detect for us if the first name is
      // a pattern. And if it is, to refrain from adding pair/dir/type.
      //
      optional<const target_type*> pat_tt (
        parse_names (
          t, tt,
          r,
          pmode == pattern_mode::expand ? pattern_mode::detect : pmode,
          false /* chunk */,
          what,
          separators,
          0,                    // Handled by the splice_names() call below.
          pp, dp, tp,
          false /* cross */,
          true  /* curly */).pattern);

      if (tt != type::rcbrace)
        fail (t) << "expected '}' instead of " << t;

      // See if this is a pattern.
      //
      if (pat_tt)
      {
        // In the pre-parse mode the parse_names() result can never be a
        // pattern.
        //
        assert (!pre_parse_);

        // Move the pattern names our of the result.
        //
        names ps;
        if (start == 0)
          ps = move (r);
        else
          ps.insert (ps.end (),
                     make_move_iterator (r.begin () + start),
                     make_move_iterator (r.end ()));
        r.resize (start);

        expand_name_pattern (loc, move (ps), r, what, 0, dp, tp, *pat_tt);
      }
    };

    // Parse and expand the first group.
    //
    parse (pp, dp, tp);

    // Handle crosses. The overall plan is to take what's in r, cross each
    // element with the next group using the re-parse machinery, and store the
    // result back to r.
    //
    while (cross && peek () == type::lcbrace && !peeked ().separated)
    {
      next (t, tt); // Get '{'.

      names ln (move (r));
      r.clear ();

      // Cross with empty LHS/RHS is empty. Handle the LHS case now by parsing
      // and discaring RHS (empty RHS is handled "naturally" below).
      //
      if (ln.size () == 0)
      {
        next (t, tt); // Get what's after '{'.
        parse (nullopt, nullptr, nullptr);
        r.clear ();
        continue;
      }

      // In the pre-parse mode we fall back to the above "cross with empty
      // LHS" case.
      //
      assert (!pre_parse_);

      //@@ This can be a nested replay (which we don't support), for example,
      //   via target-specific var assignment. Add support for nested (2-level
      //   replay)? Why not use replay_guard for storage? Alternatively, don't
      //   use it here (see parse_for() for an alternative approach).
      //
      replay_guard rg (*this, ln.size () > 1);
      for (auto i (ln.begin ()), e (ln.end ()); i != e; )
      {
        next (t, tt); // Get what's after '{'.
        const location loc (get_location (t));

        name& l (*i);

        // "Promote" the lhs value to type.
        //
        if (!l.value.empty ())
        {
          if (!l.type.empty ())
            fail (loc) << "nested type name " << l.value;

          l.type.swap (l.value);
        }

        parse (l.proj,
               l.dir.empty () ? nullptr : &l.dir,
               l.type.empty () ? nullptr : &l.type);

        if (++i != e)
          rg.play (); // Replay.
      }
    }

    // We don't modify the resulting names during pre-parsing and so can bail
    // out now.
    //
    if (pre_parse_)
      return 0;

    // Splice the names into the result. Note that we have already handled
    // project/dir/type qualification but may still have a pair. Fast-path
    // common cases.
    //
    if (pairn == 0)
    {
      if (start == 0)
        ns = move (r);
      else
        ns.insert (ns.end (),
                   make_move_iterator (r.begin ()),
                   make_move_iterator (r.end ()));
    }
    else
      splice_names (loc,
                    names_view (r), move (r),
                    ns, what,
                    pairn,
                    nullopt, nullptr, nullptr);

    return ns.size () - start;
  }

  bool parser::
  start_names (type& tt, bool lp)
  {
    return (tt == type::word           ||
            tt == type::lcbrace        ||  // Untyped name group: '{foo ...'.
            tt == type::dollar         ||  // Variable expansion: '$foo ...'.
            (tt == type::lparen && lp) ||  // Eval context: '(foo) ...'.
            tt == type::pair_separator);   // Empty pair LHS: '@foo ...'.
  }

  // Slashe(s) plus '%'. Note that here we assume '/' is there since that's
  // in our buildfile "syntax".
  //
  const string parser::name_separators (
    string (path::traits_type::directory_separators) + '%');

  auto parser::
  parse_names (token& t, type& tt,
               names& ns,
               pattern_mode pmode,
               bool chunk,
               const char* what,
               const string* separators,
               size_t pairn,
               const optional<project_name>& pp,
               const dir_path* dp,
               const string* tp,
               bool cross,
               bool curly) -> parse_names_result
  {
    // Note that support for pre-parsing is partial, it does not handle
    // groups ({}).
    //
    // If pairn is not 0, then it is an index + 1 of the first half of the
    // pair for which we are parsing the second halves, for example:
    //
    // a@{b c d{e f} {}}

    tracer trace ("parser::parse_names", &path_);

    if (pp)
      pmode = pattern_mode::preserve;

    // Returned value NULL/type and pattern (see below).
    //
    bool rvalue (false);
    bool vnull (false);
    const value_type* vtype (nullptr);
    optional<const target_type*> rpat;

    // Buffer that is used to collect the complete name in case of an
    // unseparated variable expansion or eval context, e.g., foo$bar($baz)fox.
    // The idea is to concatenate all the individual parts in this buffer and
    // then re-inject it into the loop as a single token.
    //
    // If the concatenation is untyped (see below), then the name should be
    // simple (i.e., just a string).
    //
    bool concat (false);
    bool concat_quoted (false);
    bool concat_quoted_first (false);
    name concat_data;

    auto concat_diag_multiple = [this] (const location& loc,
                                        const char* what_expansion)
    {
      diag_record dr (fail (loc));

      dr << "concatenating " << what_expansion << " contains multiple values";

      // See if this looks like a subscript without an evaluation context and
      // help the user out.
      //
      if (mode () != lexer_mode::eval)
      {
        const token& t (peeked ()); // Should be peeked at.

        if (t.type == type::word &&
            t.qtype == quote_type::unquoted &&
            t.value[0] == '[')
        {
          dr << info << "wrap it in (...) evaluation context if this "
             << "is value subscript";
        }
      }
    };

    auto concat_typed = [this, what, &vnull, &vtype,
                         &concat, &concat_data,
                         &concat_diag_multiple] (value&& rhs,
                                                 const location& loc,
                                                 const char* what_expansion)
    {
      // If we have no LHS yet, then simply copy value/type.
      //
      if (concat)
      {
        small_vector<value, 2> a;

        // Convert LHS to value.
        //
        a.push_back (value (vtype)); // Potentially typed NULL value.

        if (!vnull)
          a.back ().assign (move (concat_data), nullptr);

        // RHS.
        //
        // Note that if RHS contains multiple values then we expect the result
        // to be a single value somehow or, more likely, there to be no
        // suitable $builtin.concat() overload.
        //
        a.push_back (move (rhs));

        const char* l ((a[0].type != nullptr ? a[0].type->name : "<untyped>"));
        const char* r ((a[1].type != nullptr ? a[1].type->name : "<untyped>"));

        pair<value, bool> p;
        {
          // Print the location information in case the function fails.
          //
          auto df = make_diag_frame (
            [this, &loc, l, r] (const diag_record& dr)
            {
              dr << info (loc) << "while concatenating " << l << " to " << r;
              dr << info << "use quoting to force untyped concatenation";
            });

          if (ctx == nullptr)
            fail << "literal " << what << " expected";

          p = ctx->functions.try_call (
            scope_, "builtin.concat", vector_view<value> (a), loc);
        }

        if (!p.second)
          fail (loc) << "no typed concatenation of " << l << " to " << r <<
            info << "use quoting to force untyped concatenation";

        rhs = move (p.first);

        // It seems natural to expect that a typed concatenation result
        // is also typed.
        //
        assert (rhs.type != nullptr);
      }

      vnull = rhs.null;
      vtype = rhs.type;

      if (!vnull)
      {
        if (vtype != nullptr)
          untypify (rhs, true /* reduce */);

        names& d (rhs.as<names> ());

        // If the value is empty, then we asked untypify() to reduce it to
        // an empty sequence of names rather than a sequence of one empty
        // name.
        //
        if (size_t n = d.size ())
        {
          if (n != 1)
          {
            assert (what_expansion != nullptr);
            concat_diag_multiple (loc, what_expansion);
          }

          concat_data = move (d[0]);
        }
      }
    };

    // Set the result pattern target type and switch to the preserve mode.
    //
    // The goal of the detect mode is to assemble the "raw" list (the pattern
    // itself plus inclusions/exclusions) that will then be passed to
    // expand_name_pattern(). So clear pair, directory, and type (they will be
    // added during pattern expansion) and change the mode to preserve (to
    // prevent any expansions in inclusions/exclusions).
    //
    auto pattern_detected =
      [&pairn, &dp, &tp, &rpat, &pmode] (const target_type* ttp)
    {
      assert (pmode == pattern_mode::detect);

      pairn = 0;
      dp = nullptr;
      tp = nullptr;
      pmode = pattern_mode::preserve;
      rpat = ttp;
    };

    // Return '+' or '-' if a token can start an inclusion or exclusion
    // (pattern or group), '\0' otherwise. The result can be used as bool.
    // Note that token::qfirst covers both quoting and escaping.
    //
    auto pattern_prefix = [] (const token& t) -> char
    {
      char c;
      return (t.type == type::word && !t.qfirst &&
              ((c = t.value[0]) == '+' || c == '-')
              ? c
              : '\0');
    };

    // A name sequence potentially starts with a pattern if it starts with a
    // literal unquoted plus character.
    //
    bool ppat (pmode == pattern_mode::detect && pattern_prefix (t) == '+');

    // Potential pattern inclusion group. To be recognized as such it should
    // start with the literal unquoted '+{' string and expand into a non-empty
    // name sequence.
    //
    // The first name in such a group is a pattern, regardless of whether it
    // contains wildcard characters or not. The trailing names are inclusions.
    // For example the following pattern groups are equivalent:
    //
    // cxx{+{f* *oo}}
    // cxx{f* +*oo}
    //
    bool pinc (ppat && t.value == "+" &&
               peek () == type::lcbrace && !peeked ().separated);

    // Number of names in the last group. This is used to detect when
    // we need to add an empty first pair element (e.g., @y) or when
    // we have a (for now unsupported) multi-name LHS (e.g., {x y}@z).
    //
    size_t count (0);
    size_t start (ns.size ());

    for (bool first (true);; first = false)
    {
      // Note that here we assume that, except for the first iterartion,
      // tt contains the type of the peeked token.

      // Automatically reset the detect pattern mode to expand after the
      // first element.
      //
      if (pmode == pattern_mode::detect && start != ns.size ())
        pmode = pattern_mode::expand;

      // Return true if the next token (which should be peeked at) won't be
      // part of the name.
      //
      auto last_token = [chunk, this] ()
      {
        const token& t (peeked ());
        type tt (t.type);

        return ((chunk && t.separated) || !start_names (tt));
      };

      // Return true if the next token (which should be peeked at) won't be
      // part of this concatenation. The et argument can be used to recognize
      // an extra (unseparated) token type as being concatenated.
      //
      auto last_concat = [this] (type et = type::eos)
      {
        const token& t (peeked ());
        type tt (t.type);

        return (t.separated         ||
                (tt != type::word   &&
                 tt != type::dollar &&
                 tt != type::lparen &&
                 (et == type::eos ? true : tt != et)));
      };

      // If we have accumulated some concatenations, then we have two options:
      // continue accumulating or inject. We inject if the next token is not a
      // word, var expansion, or eval context or if it is separated.
      //
      optional<pair<const value_type*, name>> path_concat; // Backup.

      if (concat && last_concat ())
      {
        // Concatenation does not affect the tokens we get, only what we do
        // with them. As a result, we never set the concat flag during pre-
        // parsing.
        //
        assert (!pre_parse_);

        bool quoted (concat_quoted);
        bool quoted_first (concat_quoted_first);

        concat = false;
        concat_quoted = false;
        concat_quoted_first = false;

        // If this is a result of typed concatenation, then don't inject. For
        // one we don't want any of the "interpretations" performed in the
        // word parsing code below.
        //
        // And if this is the only name, then we also want to preserve the
        // type in the result.
        //
        // There is one exception, however: if the type is path, dir_path, or
        // string and what follows is an unseparated '{', then we need to
        // untypify it and inject in order to support our directory/target-
        // type syntax (this means that a target type must be a valid path
        // component). For example:
        //
        //   $out_root/foo/lib{bar}
        //   $out_root/$libtype{bar}
        //
        // And here is another exception: if we have a project, directory, or
        // type, then this is a name and we should also untypify it (let's for
        // now do it for the same set of types as the first exception). For
        // example:
        //
        //   dir/{$str}
        //   file{$str}
        //
        // And yet another exception: if the type is path or dir_path and the
        // pattern mode is not ignore, then we will inject to try our luck in
        // interpreting the concatenation result as a path pattern. This makes
        // sure patterns like `$src_base/*.txt` work, naturally. Failed that,
        // we will handle this concatenation as we do for other types (via the
        // path_concat backup).
        //

        // A concatenation cannot produce value/NULL.
        //
        vnull = false;
        rvalue = false;

        if (vtype != nullptr)
        {
          bool e1 (tt == type::lcbrace && !peeked ().separated);
          bool e2 (pp || dp != nullptr || tp != nullptr);

          const value_type* pt (&value_traits<path>::value_type);
          const value_type* dt (&value_traits<dir_path>::value_type);

          if (e1 || e2)
          {
            if (vtype == pt || vtype == &value_traits<string>::value_type)
              ; // Representation is already in concat_data.value.
            else if (vtype == dt)
              concat_data.value = move (concat_data.dir).representation ();
            else
            {
              diag_record dr (fail (t));

              if      (e1) dr << "expected directory and/or target type";
              else if (e2) dr << "expected name";

              dr << " instead of " << vtype->name << endf;
            }

            vtype = nullptr;
            // Fall through to injection.
          }
          else if (pmode != pattern_mode::ignore &&
                   (vtype == pt || vtype == dt))
          {
            path_concat = make_pair (vtype, concat_data);

            // Note: for path the representation is already in
            // concat_data.value.
            //
            if (vtype == dt)
              concat_data.value = move (concat_data.dir).representation ();

            vtype = nullptr;
            // Fall through to injection.
          }
          else
          {
            // This is either a simple name (untyped concatenation; in which
            // case it is always valid) or it came from type concatenation in
            // which case we can assume the result is valid.
            //
            ns.push_back (move (concat_data));

            // Clear the type information if that's not the only name.
            //
            if (start != ns.size () || !last_token ())
              vtype = nullptr;

            // Restart the loop (but now with concat mode off) to handle
            // chunking, etc.
            //
            continue;
          }
        }

        // Replace the current token with our injection (after handling it we
        // will peek at the current token again).
        //
        // We don't know what exactly was quoted so approximating as partially
        // mixed quoted.
        //
        tt = type::word;
        t = token (move (concat_data.value),
                   true,
                   quoted ? quote_type::mixed : quote_type::unquoted,
                   false, quoted_first,
                   t.line, t.column);
      }
      else if (!first)
      {
        // If we are chunking, stop at the next separated token.
        //
        next (t, tt);

        if (chunk && t.separated)
          break;

        // If we are parsing the pattern group, then space-separated tokens
        // must start inclusions or exclusions (see above).
        //
        if (rpat && t.separated && tt != type::rcbrace && !pattern_prefix (t))
          fail (t) << "expected name pattern inclusion or exclusion";
      }

      // Name.
      //
      // A user may specify a value that is an invalid name (e.g., it contains
      // '%' but the project name is invalid). While it may seem natural to
      // expect quoting/escaping to be the answer, we may need to quote names
      // (e.g., spaces in paths) and so in our model quoted values are still
      // treated as names and we rely on reversibility if we need to treat
      // them as values. The reasonable solution to the invalid name problem is
      // then to treat them as values if they are quoted.
      //
      if (tt == type::word)
      {
        tt = peek ();

        // Skip it in the pre-parse mode (any {...} that may follow will be
        // handled as an untyped group below).
        //
        if (pre_parse_)
          continue;

        string val (move (t.value));
        const location loc (get_location (t));
        bool quoted (t.qtype != quote_type::unquoted);
        bool quoted_first (t.qfirst);

        // Should we accumulate? If the buffer is not empty, then we continue
        // accumulating (the case where we are separated should have been
        // handled by the injection code above). If the next token is a var
        // expansion or eval context and it is not separated, then we need to
        // start accumulating.
        //
        if (concat        || // Continue.
            !last_concat ()) // Start.
        {
          bool e (val.empty ());

          // If LHS is typed then do typed concatenation.
          //
          if (concat && vtype != nullptr)
          {
            // Create untyped RHS.
            //
            names ns;
            ns.push_back (name (move (val)));
            concat_typed (value (move (ns)), get_location (t), nullptr);
          }
          else
          {
            auto& v (concat_data.value);

            if (v.empty ())
              v = move (val);
            else
              v += val;
          }

          // Consider something like this: ""$foo where foo='+foo'. Should we
          // treat the plus as a first (unquoted) character? Feels like we
          // should not. The way we achieve this is a bit hackish: we make it
          // look like a quoted first character. Note that there is a second
          // half of this in expansion case which deals with $empty+foo.
          //
          if (!concat) // First.
            concat_quoted_first = quoted_first || e;

          concat_quoted = quoted || concat_quoted;
          concat = true;

          continue;
        }

        // Find a separator (slash or %).
        //
        string::size_type pos (separators != nullptr
                               ? val.find_last_of (*separators)
                               : string::npos);

        // First take care of project. A project-qualified name is not very
        // common, so we can afford some copying for the sake of simplicity.
        //
        optional<project_name> p1;
        const optional<project_name>* pp1 (&pp);

        if (pos != string::npos)
        {
          bool last (val[pos] == '%');
          string::size_type q (last ? pos : val.rfind ('%', pos - 1));

          for (; q != string::npos; ) // Breakout loop.
          {
            // Process the project name.
            //
            string proj (val, 0, q);

            try
            {
              p1 = !proj.empty ()
                ? project_name (move (proj))
                : project_name ();
            }
            catch (const invalid_argument& e)
            {
              if (quoted) // See above.
                break;

              fail (loc) << "invalid project name '" << proj << "': " << e;
            }

            if (pp)
              fail (loc) << "nested project name " << *p1;

            pp1 = &p1;

            // Now fix the rest of the name.
            //
            val.erase (0, q + 1);
            pos = last ? string::npos : pos - (q + 1);

            break;
          }
        }

        size_t size (pos != string::npos ? val.size () - 1 : 0);

        // See if this is a type name, directory prefix, or both. That
        // is, it is followed by an un-separated '{'.
        //
        if (tt == type::lcbrace && !peeked ().separated)
        {
          next (t, tt);

          // Resolve the target, if there is one, for the potential pattern
          // inclusion group. If we fail, then this is not an inclusion group.
          //
          const target_type* ttp (nullptr);

          if (pinc)
          {
            assert (val == "+");

            if (tp != nullptr && scope_ != nullptr)
            {
              ttp = scope_->find_target_type (*tp);

              if (ttp == nullptr)
                ppat = pinc = false;
              else if (ttp->factory == nullptr)
                fail (loc) << "abstract target type " << ttp->name << "{}";
            }
          }

          if (pos != size && tp != nullptr && !pinc)
            fail (loc) << "nested type name " << val;

          dir_path d1;
          const dir_path* dp1 (dp);

          string t1;
          const string* tp1 (tp);

          try
          {
            if (pos == string::npos) // type
              tp1 = &val;
            else if (pos == size) // directory
            {
              if (dp == nullptr)
                d1 = dir_path (val);
              else
                d1 = *dp / dir_path (val);

              dp1 = &d1;
            }
            else // both
            {
              t1.assign (val, pos + 1, size - pos);

              if (dp == nullptr)
                d1 = dir_path (val, 0, pos + 1);
              else
                d1 = *dp / dir_path (val, 0, pos + 1);

              dp1 = &d1;
              tp1 = &t1;
            }
          }
          catch (const invalid_path& e)
          {
            fail (loc) << "invalid path '" << e.path << "'";
          }

          count = parse_names_trailer (
            t, tt, ns, pmode, what, separators, pairn, *pp1, dp1, tp1, cross);

          // If empty group or empty name, then this is not a pattern inclusion
          // group (see above).
          //
          if (pinc)
          {
            if (count != 0 && (count > 1 || !ns.back ().empty ()))
              pattern_detected (ttp);

            ppat = pinc = false;
          }

          tt = peek ();

          continue;
        }

        // See if this is a pattern, path or regex.
        //
        // A path pattern either contains an unquoted wildcard character or,
        // in the curly context, starts with unquoted/unescaped `+`.
        //
        // A regex pattern starts with unquoted/unescaped `~` followed by a
        // non-alphanumeric delimiter and has the following form:
        //
        // ~/<pat>/[<flags>]
        //
        // A regex substitution starts with unquoted/unescaped '^' followed by
        // a non-alphanumeric delimiter and has the follwing form:
        //
        // ^/<sub>/[<flags>]
        //
        // Any non-alphanumeric character other that `/` can be used as a
        // delimiter but escaping of the delimiter character is not supported
        // (one benefit of this is that we can store and print the pattern as
        // is without worrying about escaping; the non-alphanumeric part is to
        // allow values like ~host and ^cat).
        //
        // The following pattern flags are recognized:
        //
        // i -- match ignoring case
        // e -- match including extension
        //
        // Note that we cannot express certain path patterns that start with
        // the regex introducer using quoting (for example, `~*`) since
        // quoting prevents the whole from being recognized as a path
        // pattern. However, we can achieve this with escaping (for example,
        // \~*). This works automatically since we treat (at the lexer level)
        // escaped first characters as quoted without treating the whole thing
        // as quoted. Note that there is also the corresponding logic in
        // to_stream(name).
        //
        // A pattern cannot be project-qualified.
        //
        optional<pattern_type> pat;

        if (pmode != pattern_mode::ignore && !*pp1)
        {
          // Note that in the general case we need to convert it to a path
          // prior to testing for being a pattern (think of b[a/r] that is not
          // a pattern).
          //
          auto path_pattern = [&val, &loc, this] ()
          {
            // Let's optimize it a bit for the common cases.
            //
            if (val.find_first_of ("*?[") == string::npos)
              return false;

            if (path_traits::find_separator (val) == string::npos)
              return build2::path_pattern (val);

            try
            {
              return build2::path_pattern (path (val));
            }
            catch (const invalid_path& e)
            {
              fail (loc) << "invalid path '" << e.path << "'" << endf;
            }
          };

          auto regex_pattern = [&val] ()
          {
            return ((val[0] == '~' || val[0] == '^') &&
                    val[1] != '\0' && !alnum (val[1]));
          };

          if (pmode != pattern_mode::preserve)
          {
            // Note that if we have no base directory or cannot resolve the
            // target type, then this affectively becomes the ignore mode.
            //
            if (pbase_ != nullptr || (dp != nullptr && dp->absolute ()))
            {
              // Note that we have to check for regex patterns first since
              // they may also be detected as path patterns.
              //
              if (!quoted_first && !path_concat && regex_pattern ())
              {
                // Note: we may decide to support regex-based name generation
                // some day (though a substitution won't make sense here).
                //
                fail (loc) << "regex pattern-based name generation" <<
                  info << "quote '" << val << "' (or escape first character) "
                       << "to treat it as literal name (or path pattern)";
              }
              else if ((!quoted && path_pattern ()) ||
                       (!quoted_first && curly && val[0] == '+'))
              {
                // Resolve the target type if there is one.
                //
                const target_type* ttp (tp != nullptr && scope_ != nullptr
                                        ? scope_->find_target_type (*tp)
                                        : nullptr);

                if (ttp != nullptr && ttp->factory == nullptr)
                  fail (loc) << "abstract target type " << ttp->name << "{}";

                if (tp == nullptr || ttp != nullptr)
                {
                  if (pmode == pattern_mode::detect)
                  {
                    // Strip the literal unquoted plus character for the first
                    // pattern in the group.
                    //
                    if (ppat)
                    {
                      assert (val[0] == '+');
                      val.erase (0, 1);
                      ppat = pinc = false;
                    }

                    // Set the detect pattern mode to expand if the pattern is
                    // not followed by the inclusion/exclusion pattern/match.
                    // Note that if it is '}' (i.e., the end of the group),
                    // then it is a single pattern and the expansion is what
                    // we want.
                    //
                    if (!pattern_prefix (peeked ()))
                      pmode = pattern_mode::expand;
                  }

                  if (pmode == pattern_mode::expand)
                  {
                    count = expand_name_pattern (get_location (t),
                                                 names {name (move (val))},
                                                 ns,
                                                 what,
                                                 pairn,
                                                 dp, tp, ttp);
                    continue;
                  }

                  pattern_detected (ttp);

                  // Fall through.
                }
              }
            }
          }
          else
          {
            // For the preserve mode we treat it as a pattern if it look like
            // one syntactically. For now we also don't treat leading `+` in
            // the curly context as an indication of a path pattern (since
            // there isn't any good reason to; see also to_stream(name) for
            // the corresponding serialization logic).
            //
            if (!quoted_first && !path_concat && regex_pattern ())
            {
              const char* w;
              if (val[0] == '~')
              {
                w = "regex pattern";
                pat = pattern_type::regex_pattern;
              }
              else
              {
                w = "regex substitution";
                pat = pattern_type::regex_substitution;
              }

              size_t n (val.size ());

              // Verify delimiters and find the position of the flags.
              //
              char d (val[1]);
              size_t p (val.rfind (d));

              if (p == 1)
              {
                fail (loc) << "no trailing delimiter '" << d << "' in "
                           << w << " '" << val << "'" <<
                  info << "quote '" << val << "' (or escape first character) "
                       << "to treat it as literal name (or path pattern)";
              }

              // Verify flags.
              //
              for (size_t i (++p); i != n; ++i)
              {
                char f (val[i]);

                if (*pat == pattern_type::regex_pattern)
                {
                  if (f == 'i' || f == 'e')
                    continue;
                }

                fail (loc) << "unknown flag '" << f << "' in " << w << " '"
                           << val << "'";
              }

              val.erase (0, 1); // Remove `~` or `^`.

              // Make sure we don't treat something like `~/.../` as a
              // directory.
              //
              pos = string::npos;
              size = 0;
            }
            else if (!quoted && path_pattern ())
              pat = pattern_type::path;
          }
        }

        // If this is a concatenation of the path or dir_path type and it is
        // not a pattern, then handle it in the same way as concatenations of
        // other types (see above).
        //
        if (path_concat && !pat)
        {
          ns.push_back (move (path_concat->second));

          // Restore the type information if that's the only name.
          //
          if (start == ns.size () && last_token ())
            vtype = path_concat->first;

          // Restart the loop.
          //
          continue;
        }

        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pairn != 0 && pairn != ns.size ())
          ns.push_back (ns[pairn - 1]);

        count = 1;

        // If it ends with a directory separator, then it is a directory.
        // Note that at this stage we don't treat '.' and '..' as special
        // (unless they are specified with a directory separator) because
        // then we would have ended up treating '.: ...' as a directory
        // scope. Instead, this is handled higher up the processing chain,
        // in scope::find_target_type(). This would also mess up
        // reversibility to simple name.
        //
        // Note: a regex pattern cannot be a directory (see above).
        //
        if (pos == size)
        {
          // For reversibility to simple name, only treat it as a directory
          // if the string is an exact representation.
          //
          dir_path dir (move (val), dir_path::exact);

          if (!dir.empty ())
          {
            if (dp != nullptr)
              dir = *dp / dir;

            append_name (
              ns,
              *pp1, move (dir), (tp != nullptr ? *tp : string ()), string (),
              pat, loc);

            continue;
          }
        }

        append_name (ns,
                     *pp1,
                     (dp != nullptr ? *dp : dir_path ()),
                     (tp != nullptr ? *tp : string ()),
                     move (val),
                     pat,
                     loc);

        continue;
      }

      // Expanions: variable expansion, function call, or eval context.
      //
      if (tt == type::dollar || tt == type::lparen)
      {
        if (ctx == nullptr)
          fail << "literal " << what << " expected";

        // These cases are pretty similar in that in both we quickly end up
        // with a list of names that we need to splice into the result.
        //
        location loc;
        value result_data;
        const value* result (&result_data);
        const char* what; // Variable, function, or evaluation context.
        bool quoted (t.qtype != quote_type::unquoted);

        // We only recognize value subscripts inside eval contexts due to the
        // ambiguity with wildcard patterns (consider: $x[123].txt).
        //
        bool sub (mode () == lexer_mode::eval);

        if (tt == type::dollar)
        {
          // Switch to the variable name mode. We want to use this mode for
          // $foo but not for $(foo). Since we don't know whether the next
          // token is a paren or a word, we turn it on and switch to the eval
          // mode if what we get next is a paren.
          //
          mode (lexer_mode::variable);

          // Sniff out the special variables string from mode data and use
          // that to recognize special variables in the ad hoc $() handling
          // below.
          //
          // Note: must be done before calling next() which may expire the
          // mode.
          //
          auto special = [s = reinterpret_cast<const char*> (mode_data ())]
            (const token& t) -> char
          {
            char r ('\0');

            if (s != nullptr)
            {
              switch (t.type)
              {
              case type::less:           r = '<';        break;
              case type::greater:        r = '>';        break;
              case type::colon:          r = ':';        break;
              case type::dollar:         r = '$';        break;
              case type::question:       r = '?';        break;
              case type::comma:          r = ',';        break;
              case type::backtick:       r = '`';        break;
              case type::bit_or:         r = '|';        break;
              case type::log_not:        r = '!';        break;
              case type::lparen:         r = '(';        break;
              case type::rparen:         r = ')';        break;
              case type::lcbrace:        r = '{';        break;
              case type::rcbrace:        r = '}';        break;
              case type::lsbrace:        r = '[';        break;
              case type::rsbrace:        r = ']';        break;
              case type::pair_separator: r = t.value[0]; break;
              default:                                   break;
              }

              if (r != '\0' && strchr (s, r) == nullptr)
                r = '\0';
            }

            return r;
          };

          next (t, tt);
          loc = get_location (t);

          if (tt == type::escape)
          {
            // For now we only support all the simple C/C++ escape sequences
            // plus \0 (which in C/C++ is an octal escape sequence). See the
            // lexer part for details.
            //
            // Note: cannot be subscripted.
            //
            if (!pre_parse_)
            {
              string s;
              switch (char c = t.value[0])
              {
              case '\'':
              case '"':
              case '?':
              case '\\': s = c;    break;
              case '0':  s = '\0'; break;
              case 'a':  s = '\a'; break;
              case 'b':  s = '\b'; break;
              case 'f':  s = '\f'; break;
              case 'n':  s = '\n'; break;
              case 'r':  s = '\r'; break;
              case 't':  s = '\t'; break;
              case 'v':  s = '\v'; break;
              default:
                assert (false);
              }

              result_data = name (move (s));
              what = "escape sequence expansion";
            }

            tt = peek ();
          }
          else
          {
            names qual;
            string name;

            if (t.separated)
              ; // Leave the name empty to fail below.
            else if (tt == type::word)
            {
              name = move (t.value);
            }
            else if (tt == type::lparen)
            {
              expire_mode ();
              mode (lexer_mode::eval, '@');
              next_with_attributes (t, tt);

              // Handle the $(x) case ad hoc. We do it this way in order to
              // get the variable name even during pre-parse. It should also
              // be faster.
              //
              char c ('\0');
              if ((tt == type::word
                   ? path_traits::rfind_separator (t.value) == string::npos
                   : (c = special (t))) &&
                  peek () == type::rparen)
              {
                name = (tt == type::word ? move (t.value) : string (1, c));
                next (t, tt); // Get `)`.
              }
              else
              {
                using name_type = build2::name;

                values vs (parse_eval (t, tt, pmode));

                if (!pre_parse_)
                {
                  if (vs.size () != 1)
                    fail (loc) << "expected single variable/function name";

                  value& v (vs[0]);

                  if (!v)
                    fail (loc) << "null variable/function name";

                  names storage;
                  vector_view<name_type> ns (
                    reverse (v, storage, true /* reduce */)); // Movable.
                  size_t n (ns.size ());

                  // We cannot handle scope-qualification in the eval context
                  // as we do for target-qualification (see eval-qual) since
                  // then we would be treating all paths as qualified
                  // variables. So we have to do it here.
                  //
                  if      (n >= 2 && ns[0].pair == ':')   // $(foo: x)
                  {
                    // Note: name is first (see eval for details).
                    //
                    qual.push_back (move (ns[1]));

                    if (qual.back ().empty ())
                      fail (loc) << "empty variable/function qualification";

                    if (n > 2)
                      qual.push_back (move (ns[2]));

                    // Move name to the last position (see below).
                    //
                    swap (ns[0], ns[n - 1]);
                  }
                  else if (n == 2 && ns[0].directory ())  // $(foo/ x)
                  {
                    qual.push_back (move (ns[0]));
                    qual.back ().pair = '/';
                  }
                  else if (n > 1)
                    fail (loc) << "expected variable/function name instead of '"
                               << ns << "'";

                  // Note: checked for empty below.
                  //
                  if (!ns[n - 1].simple ())
                    fail (loc) << "expected variable/function name instead of '"
                               << ns[n - 1] << "'";

                  size_t p;
                  if (n == 1 &&                           // $(foo/x)
                      (p = path_traits::rfind_separator (ns[0].value)) !=
                      string::npos)
                  {
                    // Note that p cannot point to the last character since
                    // then it would have been a directory, not a simple name.
                    //
                    string& s (ns[0].value);

                    name = string (s, p + 1);
                    s.resize (p + 1);
                    qual.push_back (name_type (dir_path (move (s))));
                    qual.back ().pair = '/';
                  }
                  else
                    name = move (ns[n - 1].value);
                }
              }
            }
            else
              fail (t) << "expected variable/function name instead of " << t;

            if (!pre_parse_ && name.empty ())
              fail (loc) << "empty variable/function name";

            // Figure out whether this is a variable expansion with potential
            // subscript or a function call.
            //
            if (sub) enable_subscript ();
            tt = peek ();

            // Note that we require function call opening paren to be
            // unseparated; consider: $x ($x == 'foo' ? 'FOO' : 'BAR').
            //
            if (tt == type::lparen && !peeked ().separated)
            {
              // Function call.
              //
              next (t, tt); // Get '('.
              mode (lexer_mode::eval, '@');
              next_with_attributes (t, tt);

              // @@ Should we use (target/scope) qualification (of name) as
              // the context in which to call the function? Hm, interesting...
              //
              values args (parse_eval (t, tt, pmode));

              if (sub) enable_subscript ();
              tt = peek ();

              // Note that we "move" args to call().
              //
              if (!pre_parse_)
              {
                result_data = ctx->functions.call (scope_, name, args, loc);
                what = "function call";
              }
              else
                lookup_function (move (name), loc);
            }
            else
            {
              // Variable expansion.
              //
              lookup l (lookup_variable (move (qual), move (name), loc));

              if (!pre_parse_)
              {
                if (l.defined ())
                  result = l.value; // Otherwise leave as NULL result_data.

                what = "variable expansion";
              }
            }
          }
        }
        else
        {
          // Evaluation context.
          //
          loc = get_location (t);
          mode (lexer_mode::eval, '@');
          next_with_attributes (t, tt);

          values vs (parse_eval (t, tt, pmode));

          if (sub) enable_subscript ();
          tt = peek ();

          if (!pre_parse_)
          {
            switch (vs.size ())
            {
            case 0:  result_data = value (names ()); break;
            case 1:  result_data = move (vs[0]); break;
            default: fail (loc) << "expected single value";
            }

            what = "context evaluation";
          }
        }

        // Handle value subscript.
        //
        if (mode () == lexer_mode::eval) // Note: not if(sub)!
        {
          while (tt == type::lsbrace)
          {
            location bl (get_location (t));
            next (t, tt); // `[`
            mode (lexer_mode::subscript, '\0' /* pair */);
            next (t, tt);

            location l (get_location (t));
            value v (
              tt != type::rsbrace
              ? parse_value (t, tt, pattern_mode::ignore, "value subscript")
              : value (names ()));

            if (tt != type::rsbrace)
            {
              // Note: wildcard pattern should have `]` as well so no escaping
              // suggestion.
              //
              fail (t) << "expected ']' instead of " << t;
            }

            if (!pre_parse_)
            {
              // For type-specific subscript implementations we pass the
              // subscript value as is.
              //
              if (auto f = (result->type != nullptr
                            ? result->type->subscript
                            : nullptr))
              {
                result_data = f (*result, &result_data, move (v), l, bl);
              }
              else
              {
                uint64_t j;
                try
                {
                  j = convert<uint64_t> (move (v));
                }
                catch (const invalid_argument& e)
                {
                  fail (l)    << "invalid value subscript: " << e <<
                    info (bl) << "use the '\\[' escape sequence if this is a "
                              << "wildcard pattern" << endf;
                }

                // Similar to expanding an undefined variable, we return NULL
                // if the index is out of bounds.
                //
                // Note that result may or may not point to result_data.
                //
                if (result->null)
                  result_data = value ();
                else if (result->type == nullptr)
                {
                  const names& ns (result->as<names> ());

                  // Pair-aware subscript.
                  //
                  names r;
                  for (auto i (ns.begin ()); i != ns.end (); ++i, --j)
                  {
                    if (j == 0)
                    {
                      r.push_back (*i);
                      if (i->pair)
                        r.push_back (*++i);
                      break;
                    }

                    if (i->pair)
                      ++i;
                  }

                  result_data = r.empty () ? value () : value (move (r));
                }
                else
                {
                  // Similar logic to parse_for().
                  //
                  const value_type* etype (result->type->element_type);

                  value val (result == &result_data
                             ? value (move (result_data))
                             : value (*result));

                  untypify (val, false /* reduce */);

                  names& ns (val.as<names> ());

                  // Pair-aware subscript.
                  //
                  names r;
                  for (auto i (ns.begin ()); i != ns.end (); ++i, --j)
                  {
                    bool p (i->pair);

                    if (j == 0)
                    {
                      r.push_back (move (*i));
                      if (p)
                        r.push_back (move (*++i));
                      break;
                    }

                    if (p)
                      ++i;
                  }

                  result_data = r.empty () ? value () : value (move (r));

                  if (etype != nullptr)
                    typify (result_data, *etype, nullptr /* var */);
                }
              }

              result = &result_data;
            }

            // See if we have chained subscript.
            //
            enable_subscript ();
            tt = peek ();
          }
        }

        if (pre_parse_)
          continue; // As if empty result.

        // Should we accumulate? If the buffer is not empty, then we continue
        // accumulating (the case where we are separated should have been
        // handled by the injection code above). If the next token is a word
        // or an expansion and it is not separated, then we need to start
        // accumulating. We also reduce the $var{...} case to concatention
        // and injection.
        //
        if (concat                     || // Continue.
            !last_concat (type::lcbrace)) // Start.
        {
          // This can be a typed or untyped concatenation. The rules that
          // determine which one it is are as follows:
          //
          // 1. Determine if to preserver the type of RHS: if its first
          //    token is quoted, then we do not.
          //
          // 2. Given LHS (if any) and RHS we do typed concatenation if
          //    either is typed.
          //
          // Here are some interesting corner cases to meditate on:
          //
          // $dir/"foo bar"
          // $dir"/foo bar"
          // "foo"$dir
          // "foo""$dir"
          // ""$dir
          //

          // First if RHS is typed but quoted then convert it to an untyped
          // string.
          //
          // Conversion to an untyped string happens differently, depending
          // on whether we are in a quoted or unquoted context. In an
          // unquoted context we use $representation() which must return a
          // "round-trippable representation" (and if that it not possible,
          // then it should not be overloaded for a type). In a quoted
          // context we use $string() which returns a "canonical
          // representation" (e.g., a directory path without a trailing
          // slash). Note: looks like we use typed $concat() now in the
          // unquoted context.
          //
          if (result->type != nullptr && quoted)
          {
            // RHS is already a value but it could be a const reference (to
            // the variable value) while we need to move things around. So in
            // this case we make a copy.
            //
            if (result != &result_data)
              result = &(result_data = *result);

            const char* t (result_data.type->name);

            pair<value, bool> p;
            {
              // Print the location information in case the function fails.
              //
              auto df = make_diag_frame (
                [this, &loc, t] (const diag_record& dr)
                {
                  dr << info (loc) << "while converting " << t << " to string";
                });

              if (ctx == nullptr)
                fail << "literal " << what << " expected";

              p = ctx->functions.try_call (
                scope_, "string", vector_view<value> (&result_data, 1), loc);
            }

            if (!p.second)
              fail (loc) << "no string conversion for " << t;

            result_data = move (p.first);

            // Convert to untyped simple name reducing empty string to empty
            // names as an optimization.
            //
            untypify (result_data, true /* reduce */);
          }

          if ((concat && vtype != nullptr) || // LHS typed.
              (result->type != nullptr))      // RHS typed.
          {
            if (result != &result_data) // Same reason as above.
              result = &(result_data = *result);

            concat_typed (move (result_data), loc, what);
          }
          //
          // Untyped concatenation. Note that if RHS is NULL/empty, we still
          // set the concat flag.
          //
          else if (!result->null)
          {
            // This can only be an untyped value.
            //
            // @@ Could move if result == &result_data.
            //
            const names& lv (cast<names> (*result));

            if (size_t s = lv.size ())
            {
              // This should be a simple value or a simple directory.
              //
              if (s > 1)
                concat_diag_multiple (loc, what);

              const name& n (lv[0]);

              if (n.qualified ())
                fail (loc) << "concatenating " << what << " contains project "
                           << "name";

              if (n.typed ())
                fail (loc) << "concatenating " << what << " contains target type";

              if (!n.dir.empty ())
              {
                if (!n.value.empty ())
                  fail (loc) << "concatenating " << what << " contains "
                             << "directory";

                // Note that here we cannot assume what's in dir is really a
                // path (think s/foo/bar/) so we have to reverse it exactly.
                //
                concat_data.value += n.dir.representation ();
              }
              else
                concat_data.value += n.value;
            }
          }

          // The same little hack as in the word case ($empty+foo).
          //
          if (!concat) // First.
            concat_quoted_first = true;

          concat_quoted = quoted || concat_quoted;
          concat = true;
        }
        else
        {
          // See if we should propagate the value NULL/type. We only do this
          // if this is the only expansion, that is, it is the first and the
          // next token is not part of the name.
          //
          if (first && last_token ())
          {
            vnull = result->null;
            vtype = result->type;
            rvalue = true;
          }

          // Nothing else to do here if the result is NULL or empty.
          //
          // Note that we cannot use value::empty() here since we are
          // interested in representationally empty.
          //
          if (!result->null)
          {
            // @@ Could move if nv is result_data; see untypify().
            //
            // Nuance: we should only be reducing empty simple value to empty
            // list if we are not a second half of a pair.
            //
            bool pair (!ns.empty () && ns.back ().pair);

            names nv_storage;
            names_view nv (reverse (*result, nv_storage, !pair /* reduce */));

            if (!nv.empty ())
            {
              count = splice_names (
                loc, nv, move (nv_storage), ns, what, pairn, pp, dp, tp);
            }
          }
        }

        continue;
      }

      // Untyped name group without a directory prefix, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        count = parse_names_trailer (
          t, tt, ns, pmode, what, separators, pairn, pp, dp, tp, cross);
        tt = peek ();
        continue;
      }

      // A pair separator.
      //
      if (tt == type::pair_separator)
      {
        if (pairn != 0)
          fail (t) << "nested pair on the right hand side of a pair";

        tt = peek ();

        if (!pre_parse_)
        {
          // Catch double pair separator ('@@'). Maybe we can use for
          // something later (e.g., escaping).
          //
          if (!ns.empty () && ns.back ().pair)
            fail (t) << "double pair separator";

          if (t.separated || count == 0)
          {
            // Empty LHS, (e.g., @y), create an empty name. The second test
            // will be in effect if we have something like v=@y.
            //
            append_name (ns,
                         pp,
                         (dp != nullptr ? *dp : dir_path ()),
                         (tp != nullptr ? *tp : string ()),
                         string (),
                         nullopt, /* pattern */
                         get_location (t));
            count = 1;
          }
          else if (count > 1)
            fail (t) << "multiple " << what << "s on the left hand side "
                     << "of a pair";

          ns.back ().pair = t.value[0];

          // If the next token is separated, then we have an empty RHS. Note
          // that the case where it is not a name/group (e.g., a newline/eos)
          // is handled below, once we are out of the loop.
          //
          if (peeked ().separated)
          {
            append_name (ns,
                         pp,
                         (dp != nullptr ? *dp : dir_path ()),
                         (tp != nullptr ? *tp : string ()),
                         string (),
                         nullopt, /* pattern */
                         get_location (t));
            count = 0;
          }
        }

        continue;
      }

      // Note: remember to update last_token() test if adding new recognized
      // tokens.

      if (!first)
        break;

      if (tt == type::rcbrace) // Empty name, e.g., {}.
      {
        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pairn != 0 && pairn != ns.size ())
          ns.push_back (ns[pairn - 1]);

        append_name (ns,
                     pp,
                     (dp != nullptr ? *dp : dir_path ()),
                     (tp != nullptr ? *tp : string ()),
                     string (),
                     nullopt, /* pattern */
                     get_location (t));
        break;
      }
      else
        // Our caller expected this to be something.
        //
        fail (t) << "expected " << what << " instead of " << t;
    }

    // Handle the empty RHS in a pair, (e.g., y@).
    //
    if (!ns.empty () && ns.back ().pair)
    {
      append_name (ns,
                   pp,
                   (dp != nullptr ? *dp : dir_path ()),
                   (tp != nullptr ? *tp : string ()),
                   string (),
                   nullopt, /* pattern */
                   get_location (t));
    }

    if (pre_parse_)
      assert (!rvalue && !vnull && vtype == nullptr && !rpat);

    return parse_names_result {rvalue, !vnull, vtype, rpat};
  }

  void parser::
  skip_line (token& t, type& tt)
  {
    for (; tt != type::newline && tt != type::eos; next (t, tt)) ;
  }

  void parser::
  skip_block (token& t, type& tt)
  {
    // Skip until } or eos, keeping track of the {}-balance.
    //
    for (size_t b (0); tt != type::eos; )
    {
      if (tt == type::lcbrace || tt == type::rcbrace)
      {
        type ptt (peek ());
        if (ptt == type::newline || ptt == type::eos) // Block { or }.
        {
          if (tt == type::lcbrace)
            ++b;
          else
          {
            if (b == 0)
              break;

            --b;
          }
        }
      }

      skip_line (t, tt);

      if (tt != type::eos)
        next (t, tt);
    }
  }

  bool parser::
  keyword (const token& t)
  {
    assert (replay_ != replay::play); // Can't be used in a replay.
    assert (t.type == type::word);

    // The goal here is to allow using keywords as variable names and
    // target types without imposing ugly restrictions/decorators on
    // keywords (e.g., '.using' or 'USING'). A name is considered a
    // potential keyword if:
    //
    // - it is not quoted [so a keyword can always be escaped] and
    // - next token is '\n' (or eos) or '(' [so if(...) will work] or
    // - next token is separated and is not '=', '=+', '+=', or '?=' [which
    //   means a "directive trailer" can never start with one of them].
    //
    // See tests/keyword.
    //
    if (t.qtype == quote_type::unquoted)
    {
      // We cannot peek at the whole token here since it might have to be
      // lexed in a different mode. So peek at its first character.
      //
      pair<pair<char, char>, bool> p (lexer_->peek_chars ());
      char c0 (p.first.first);
      char c1 (p.first.second);

      // Note that just checking for leading '+'/'?' is not sufficient, for
      // example:
      //
      // print +foo
      //
      // So we peek at one more character since what we expect next ('=')
      // can't be whitespace-separated.
      //
      return c0 == '\n' || c0 == '\0' || c0 == '(' ||
        (p.second                 &&
         c0 != '='                &&
         (c0 != '+' || c1 != '=') &&
         (c0 != '?' || c1 != '='));
    }

    return false;
  }

  // Buildspec parsing.
  //

  // Here is the problem: we "overload" '(' and ')' to mean operation
  // application rather than the eval context. At the same time we want to use
  // parse_names() to parse names, get variable expansion/function calls,
  // quoting, etc. We just need to disable the eval context. The way this is
  // done has two parts: Firstly, we parse names in chunks and detect and
  // handle the opening paren ourselves. In other words, a buildspec like
  // 'clean (./)' is "chunked" as 'clean', '(', etc. While this is fairly
  // straightforward, there is one snag: concatenating eval contexts, as in
  // 'clean(./)'.  Normally, this will be treated as a single chunk and we
  // don't want that. So here comes the trick (or hack, if you like): the
  // buildspec lexer mode makes every opening paren token "separated" (i.e.,
  // as if it was preceeded by a space). This will disable concatenating
  // eval.
  //
  // In fact, because this is only done in the buildspec mode, we can still
  // use eval contexts provided that we quote them: '"cle(an)"'. Note that
  // function calls also need quoting (since a separated '(' is not treated as
  // a function call): '"$identity(update)"'.
  //
  // This poses a problem, though: if it's quoted then it is a concatenated
  // expansion and therefore cannot contain multiple values, for example,
  // $identity(foo/ bar/). So what we do is disable this chunking/separation
  // after both meta-operation and operation were specified. So if we specify
  // both explicitly, then we can use eval context, function calls, etc.,
  // normally: perform(update($identity(foo/ bar/))).
  //
  buildspec parser::
  parse_buildspec (istream& is, const path_name& in)
  {
    // We do "effective escaping" of the special `'"\$(` characters (basically
    // what's escapable inside a double-quoted literal plus the single quote;
    // note, however, that we exclude line continuations and `)` since they
    // would make directory paths on Windows unusable).
    //
    path_ = &in;
    lexer l (is, *path_, 1 /* line */, "\'\"\\$(");
    lexer_ = &l;

    root_ = &ctx->global_scope.rw ();
    scope_ = root_;
    target_ = nullptr;
    prerequisite_ = nullptr;

    pbase_ = &work; // Use current working directory.

    // Turn on the buildspec mode/pairs recognition with '@' as the pair
    // separator (e.g., src_root/@out_root/exe{foo bar}).
    //
    mode (lexer_mode::buildspec, '@');

    token t;
    type tt;
    next (t, tt);

    buildspec r (tt != type::eos
                 ? parse_buildspec_clause (t, tt)
                 : buildspec ());

    if (tt != type::eos)
      fail (t) << "expected operation or target instead of " << t;

    return r;
  }

  static bool
  opname (const name& n)
  {
    // First it has to be a non-empty simple name.
    //
    if (n.pair || !n.simple () || n.empty ())
      return false;

    // Like C identifier but with '-' instead of '_' as the delimiter.
    //
    for (size_t i (0); i != n.value.size (); ++i)
    {
      char c (n.value[i]);
      if (c != '-' && !(i != 0 ? alnum (c) : alpha (c)))
        return false;
    }

    return true;
  }

  buildspec parser::
  parse_buildspec_clause (token& t, type& tt, size_t depth)
  {
    buildspec bs;

    for (bool first (true);; first = false)
    {
      // We always start with one or more names. Eval context (lparen) only
      // allowed if quoted.
      //
      if (!start_names (tt, mode () == lexer_mode::double_quoted))
      {
        if (first)
          fail (t) << "expected operation or target instead of " << t;

        break;
      }

      const location l (get_location (t)); // Start of names.

      // This call will parse the next chunk of output and produce zero or
      // more names.
      //
      names ns (parse_names (t, tt, pattern_mode::expand, depth < 2));

      if (ns.empty ()) // Can happen if pattern expansion.
        fail (l) << "expected operation or target";

      // What these names mean depends on what's next. If it is an opening
      // paren, then they are operation/meta-operation names. Otherwise they
      // are targets.
      //
      if (tt == type::lparen) // Got by parse_names().
      {
        if (ns.empty ())
          fail (t) << "expected operation name before '('";

        for (const name& n: ns)
          if (!opname (n))
            fail (l) << "expected operation name instead of '" << n << "'";

        // Inside '(' and ')' we have another, nested, buildspec. Push another
        // mode to keep track of the depth (used in the lexer implementation
        // to decide when to stop separating '(').
        //
        mode (lexer_mode::buildspec, '@');

        next (t, tt); // Get what's after '('.
        const location l (get_location (t)); // Start of nested names.
        buildspec nbs (parse_buildspec_clause (t, tt, depth + 1));

        // Parse additional operation/meta-operation parameters.
        //
        values params;
        while (tt == type::comma)
        {
          next (t, tt);

          // Note that for now we don't expand patterns. If it turns out we
          // need this, then will probably have to be (meta-) operation-
          // specific (via pre-parse or some such).
          //
          params.push_back (tt != type::rparen
                            ? parse_value (t, tt, pattern_mode::ignore)
                            : value (names ()));
        }

        if (tt != type::rparen)
          fail (t) << "expected ')' instead of " << t;

        expire_mode ();
        next (t, tt); // Get what's after ')'.

        // Merge the nested buildspec into ours. But first determine if we are
        // an operation or meta-operation and do some sanity checks.
        //
        bool meta (false);
        for (const metaopspec& nms: nbs)
        {
          // We definitely shouldn't have any meta-operations.
          //
          if (!nms.name.empty ())
            fail (l) << "nested meta-operation " << nms.name;

          if (!meta)
          {
            // If we have any operations in the nested spec, then this mean
            // that our names are meta-operation names.
            //
            for (const opspec& nos: nms)
            {
              if (!nos.name.empty ())
              {
                meta = true;
                break;
              }
            }
          }
        }

        // No nested meta-operations means we should have a single metaopspec
        // object with empty meta-operation name.
        //
        assert (nbs.size () == 1);
        const metaopspec& nmo (nbs.back ());

        if (meta)
        {
          for (name& n: ns)
          {
            bs.push_back (nmo);
            bs.back ().name = move (n.value);
            bs.back ().params = params;
          }
        }
        else
        {
          // Since we are not a meta-operation, the nested buildspec should be
          // just a bunch of targets.
          //
          assert (nmo.size () == 1);
          const opspec& nos (nmo.back ());

          if (bs.empty () || !bs.back ().name.empty ())
            bs.push_back (metaopspec ()); // Empty (default) meta operation.

          for (name& n: ns)
          {
            bs.back ().push_back (nos);
            bs.back ().back ().name = move (n.value);
            bs.back ().back ().params = params;
          }
        }
      }
      else if (!ns.empty ())
      {
        // Group all the targets into a single operation. In other
        // words, 'foo bar' is equivalent to 'update(foo bar)'.
        //
        if (bs.empty () || !bs.back ().name.empty ())
          bs.push_back (metaopspec ()); // Empty (default) meta operation.

        metaopspec& ms (bs.back ());

        for (auto i (ns.begin ()), e (ns.end ()); i != e; ++i)
        {
          // @@ We may actually want to support this at some point.
          //
          if (i->qualified ())
            fail (l) << "expected target name instead of " << *i;

          if (opname (*i))
            ms.push_back (opspec (move (i->value)));
          else
          {
            // Do we have the src_base?
            //
            dir_path src_base;
            if (i->pair)
            {
              if (i->pair != '@')
                fail << "unexpected pair style in buildspec";

              if (i->typed ())
                fail (l) << "expected target src_base instead of " << *i;

              src_base = move (i->dir);

              if (!i->value.empty ())
                src_base /= dir_path (move (i->value));

              ++i;
              assert (i != e); // Got to have the second half of the pair.
            }

            if (ms.empty () || !ms.back ().name.empty ())
              ms.push_back (opspec ()); // Empty (default) operation.

            opspec& os (ms.back ());
            os.emplace_back (move (src_base), move (*i));
          }
        }
      }
    }

    return bs;
  }

  lookup parser::
  lookup_variable (names&& qual, string&& name, const location& loc)
  {
    // Note that this function can be called during execute (for example, from
    // scripts). In particular, this means we cannot use enter_{scope,target}.

    if (pre_parse_)
      return lookup ();

    tracer trace ("parser::lookup_variable", &path_);

    const scope* s (nullptr);
    const target* t (nullptr);
    const prerequisite* p (nullptr);

    // If we are qualified, it can be a scope or a target.
    //
    if (qual.empty ())
    {
      s = scope_;
      t = target_;
      p = prerequisite_;
    }
    else
    {
      // What should we do if we cannot find the qualification (scope or
      // target)? We can "fall through" to an outer scope (there is always the
      // global scope backstop), we can return NULL straight away, or we can
      // fail. It feels like in most cases unknown scope or target is a
      // mistake and doing anything other than failing is just making things
      // harder to debug.
      //
      switch (qual.front ().pair)
      {
      case '/':
        {
          assert (qual.front ().directory ());

          dir_path& d (qual.front ().dir);
          enter_scope::complete_normalize (*scope_, d);

          s = &ctx->scopes.find_out (d);

          if (s->out_path () != d)
            fail (loc) << "unknown scope " << d << " in scope-qualified "
                       << "variable " << name << " expansion" <<
              info << "did you forget to include the corresponding buildfile?";

          break;
        }
      default:
        {
          build2::name n (move (qual.front ())), o;

          if (n.pair)
            o = move (qual.back ());

          t = enter_target::find_target (*this, n, o, loc, trace);

          if (t == nullptr || !operator>= (t->decl, target_decl::implied)) // VC14
          {
            diag_record dr (fail (loc));

            dr << "unknown target " << n;

            if (n.pair && !o.dir.empty ())
              dr << '@' << o.dir;

            dr << " in target-qualified variable " << name << " expansion";
          }

          // Use the target's var_pool for good measure.
          //
          s = &t->base_scope ();

          break;
        }
      }
    }

    // Lookup.
    //
    if (const variable* pvar =
          (s != nullptr ? s : scope_)->var_pool ().find (name))
    {
      auto& var (*pvar);

      // Note: the order of the following blocks is important.

      if (p != nullptr)
      {
        // The lookup depth is a bit of a hack but should be harmless since
        // unused.
        //
        pair<lookup, size_t> r (p->vars[var], 1);

        if (!r.first.defined ())
          r = t->lookup_original (var);

        return var.overrides == nullptr
          ? r.first
          : t->base_scope ().lookup_override (var, move (r), true).first;
      }

      if (t != nullptr)
      {
        if (var.visibility > variable_visibility::target)
        {
          fail (loc) << "variable " << var << " has " << var.visibility
                     << " visibility but is expanded in target context";
        }

        return (*t)[var];
      }

      if (s != nullptr)
      {
        if (var.visibility > variable_visibility::scope)
        {
          fail (loc) << "variable " << var << " has " << var.visibility
                     << " visibility but is expanded in scope context";
        }

        return (*s)[var];
      }
    }

    return lookup ();
  }

  void parser::
  lookup_function (string&&, const location&)
  {
    assert (pre_parse_);
  }

  auto_project_env parser::
  switch_scope (const dir_path& d)
  {
    tracer trace ("parser::switch_scope", &path_);

    auto_project_env r;

    // Switching the project during bootstrap can result in bizarre nesting
    // with unexpected loading order (e.g., config.build are loaded from inner
    // to outter rather than the expected reverse). On the other hand, it can
    // be handy to assign a variable for a nested scope in config.build. So
    // for this stage we are going to switch the scope without switching the
    // project expecting the user to know what they are doing.
    //
    bool proj (stage_ != stage::boot);

    auto p (build2::switch_scope (*root_, d, proj));
    scope_ = &p.first;
    pbase_ = scope_->src_path_ != nullptr ? scope_->src_path_ : &d;

    if (proj && p.second != root_)
    {
      root_ = p.second;

      if (root_ != nullptr)
        r = auto_project_env (*root_);

      l5 ([&]
          {
            if (root_ != nullptr)
              trace << "switching to root scope " << *root_;
            else
              trace << "switching to out of project scope";
          });
    }

    return r;
  }

  // file.cxx
  //
  extern const dir_path std_export_dir;
  extern const dir_path alt_export_dir;

  void parser::
  process_default_target (token& t, const buildfile* bf)
  {
    tracer trace ("parser::process_default_target", &path_);

    // The logic is as follows: if we have an explicit current directory
    // target, then that's the default target. Otherwise, we take the first
    // target and use it as a prerequisite to create an implicit current
    // directory target, effectively making it the default target via an
    // alias. If this is a project root buildfile, then also add exported
    // buildfiles. And if there are no targets in this buildfile, then we
    // don't do anything (reasonably assuming it's not root).
    //
    if (default_target_ == nullptr) // No targets in this buildfile.
      return;

    target* ct (
      const_cast<target*> (                     // Ok (serial execution).
        ctx->targets.find (dir::static_type,    // Explicit current dir target.
                           scope_->out_path (),
                           dir_path (),         // Out tree target.
                           string (),
                           nullopt,
                           trace)));

    if (ct != nullptr && ct->decl == target_decl::real)
      ; // Existing and not implied.
    else
    {
      target& dt (*default_target_);

      if (ct == nullptr)
      {
        l5 ([&]{trace (t) << "creating current directory alias for " << dt;});

        // While this target is not explicitly mentioned in the buildfile, we
        // say that we behave as if it were. Thus not implied.
        //
        ct = &ctx->targets.insert (dir::static_type,
                                   scope_->out_path (),
                                   dir_path (),
                                   string (),
                                   nullopt,
                                   target_decl::real,
                                   trace).first;
      }
      else
        ct->decl = target_decl::real;

      ct->prerequisites_state_.store (2, memory_order_relaxed);
      ct->prerequisites_.push_back (prerequisite (dt));
    }

    // See if this is a root buildfile and not in a simple project.
    //
    if (bf != nullptr                          &&
        root_ != nullptr                       &&
        root_->root_extra != nullptr           &&
        root_->root_extra->loaded              &&
        *root_->root_extra->project != nullptr &&
        bf->dir == root_->src_path ()          &&
        bf->name == root_->root_extra->buildfile_file.string ())
    {
      // See if we have any exported buildfiles.
      //
      const dir_path& export_dir (
        root_->root_extra->altn ? alt_export_dir : std_export_dir);

      dir_path d (root_->src_path () / export_dir);
      if (exists (d))
      {
        // Make sure prerequisites are set.
        //
        ct->prerequisites_state_.store (2, memory_order_relaxed);

        const string& build_ext (root_->root_extra->build_ext);

        // Return true if entered any exported buildfiles.
        //
        // Note: recursive lambda.
        //
        auto iterate = [this, &trace,
                        ct, &build_ext] (const dir_path& d,
                                         const auto& iterate) -> bool
        {
          bool r (false);

          try
          {
            for (const dir_entry& e:
                   dir_iterator (d, dir_iterator::detect_dangling))
            {
              switch (e.type ())
              {
              case entry_type::directory:
              {
                r = iterate (d / path_cast<dir_path> (e.path ()), iterate) || r;
                break;
              }
              case entry_type::regular:
              {
                const path& n (e.path ());

                // Besides the buildfile also export buildscript and C++ files
                // that are used to provide recipe implementations (see
                // parse_recipe() for details).
                //
                string e (n.extension ());
                if (const target_type* tt = (
                      e == build_ext     ? &buildfile::static_type   :
                      e == "buildscript" ? &buildscript::static_type :
                      e == "cxx" ||
                      e == "cpp" ||
                      e == "cc"          ? &file::static_type : nullptr))
                {
                  // Enter as if found by search_existing_file(). Note that
                  // entering it as real would cause file_rule not to match
                  // for clean.
                  //
                  // Note that these targets may already be entered (for
                  // example, if already imported).
                  //
                  const target& bf (
                    ctx->targets.insert (*tt,
                                         d,
                                         (root_->out_eq_src ()
                                          ? dir_path ()
                                          : out_src (d, *root_)),
                                         n.base ().string (),
                                         move (e),
                                         target_decl::prereq_file,
                                         trace).first);

                  ct->prerequisites_.push_back (prerequisite (bf));
                  r = true;
                }

                break;
              }
              case entry_type::unknown:
              {
                bool sl (e.ltype () == entry_type::symlink);

                fail << (sl ? "dangling symlink" : "inaccessible entry")
                     << ' ' << d / e.path ();

                break;
              }
              default:
                break;
              }
            }
          }
          catch (const system_error& e)
          {
            fail << "unable to iterate over " << d << ": " << e;
          }

          return r;
        };

        if (iterate (d, iterate))
        {
          // Arrange for the exported buildfiles to be installed, recreating
          // subdirectories inside export/. Essentially, we are arranging for
          // this:
          //
          // build/export/file{*}:
          // {
          //   install = buildfile/
          //   install.subdirs = true
          // }
          //
          if (cast_false<bool> (root_->vars["install.loaded"]))
          {
            enter_scope es (*this, dir_path (export_dir));
            auto& vars (scope_->target_vars[file::static_type]["*"]);

            // @@ TODO: get cached variables from the module once we have one.
            //
            {
              auto r (vars.insert (*root_->var_pool ().find ("install")));

              if (r.second) // Already set by the user?
                r.first = path_cast<path> (dir_path ("buildfile"));
            }

            {
              auto r (vars.insert (
                        *root_->var_pool (true).find ("install.subdirs")));
              if (r.second)
                r.first = true;
            }
          }
        }
      }
    }
  }

  template <typename T>
  const T& parser::
  enter_buildfile (const path& p, optional<dir_path> out)
  {
    tracer trace ("parser::enter_buildfile", &path_);

    dir_path d (p.directory ()); // Empty for a path name with the NULL path.

    // Figure out if we need out.
    //
    dir_path o;
    if (out)
      o = move (*out);
    else if (root_ != nullptr            &&
             root_->src_path_ != nullptr &&
             !root_->out_eq_src ()       &&
             d.sub (*root_->src_path_))
    {
      o = out_src (d, *root_);
    }

    return ctx->targets.insert<T> (
      move (d),
      move (o),
      p.leaf ().base ().string (),
      p.extension (), // Always specified.
      trace);
  }

  type parser::
  next (token& t, type& tt)
  {
    replay_token r;

    if (peeked_)
    {
      r = move (peek_);
      peeked_ = false;
    }
    else
      r = replay_ != replay::play ? lexer_next () : replay_next ();

    if (replay_ == replay::save)
      replay_data_.push_back (r);

    t = move (r.token);
    tt = t.type;
    return tt;
  }

  inline type parser::
  next_after_newline (token& t, type& tt, char a)
  {
    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
    {
      diag_record dr (fail (t));
      dr << "expected newline instead of " << t;

      if (a != '\0')
        dr << " after '" << a << "'";
    }

    return tt;
  }

  inline type parser::
  next_after_newline (token& t, type& tt, const char* a)
  {
    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
    {
      diag_record dr (fail (t));
      dr << "expected newline instead of " << t;

      if (a != nullptr)
        dr << " after " << a;
    }

    return tt;
  }

  inline type parser::
  next_after_newline (token& t, type& tt, const token& a)
  {
    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
    {
      diag_record dr (fail (t));
      dr << "expected newline instead of " << t << " after " << a;
    }

    return tt;
  }

  type parser::
  peek ()
  {
    if (!peeked_)
    {
      peek_ = (replay_ != replay::play ? lexer_next () : replay_next ());
      peeked_ = true;
    }

    return peek_.token.type;
  }
}
