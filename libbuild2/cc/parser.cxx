// file      : libbuild2/cc/parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/parser.hxx>

#include <libbuild2/cc/lexer.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using type = token_type;

    void parser::
    parse (ifdstream& is, const path_name& in, unit& u, const compiler_id& cid)
    {
      cid_ = &cid;

      lexer l (is, in, true /* preprocessed */);
      l_ = &l;
      u_ = &u;

      // If the source has errors then we want the compiler to issues the
      // diagnostics. However, the errors could as likely be because we are
      // mis-parsing things. Initially, as a middle ground, we were going to
      // issue warnings. But the problem with this approach is that they are
      // easy to miss. So for now we fail. And it turns out we don't mis-
      // parse much.

      // We keep a {}-balance and skip everything at depth 1 and greater.
      // While after P1703 and P1857 everything that we are interested in
      // (module and import declarations) are treated as pseudo-pp-directives
      // and recognized everywhere, they are illegal everywhere execept at
      // depth 0. So we are going to skip for performance reasons and expect
      // the compiler to complain about the syntax rather than, say, module
      // BMI not being found.
      //
      int64_t bb (0);

      token t;
      for (bool n (true); (n ? l_->next (t) : t.type) != type::eos; )
      {
        // Break to stop, continue to continue, and set n to false if the
        // next token is already extracted.
        //
        n = true;

        switch (t.type)
        {
        case type::lcbrace:
          {
            ++bb;
            continue;
          }
        case type::rcbrace:
          {
            if (--bb < 0)
              break; // Imbalance.

            continue;
          }
        case type::identifier:
          {
            // Constructs we need to recognize:
            //
            //           module                                              ;
            // [export]  module <module-name> [<module-part>] [<attributes>] ;
            // [export]  import <module-name>                 [<attributes>] ;
            // [export]  import <module-part>                 [<attributes>] ;
            // [export]  import <header-name>                 [<attributes>] ;
            //
            // The leading module/export/import keyword should be the first
            // token of a logical line and only if certain characters appear
            // after module/import and all the tokens are on the same line,
            // then the line is recognized as a pseudo-directive; see p1857
            // for details.
            //
            // Additionally, when include is translated to an import, it's
            // normally replaced with special import (special since it may
            // appear in C context); it could be a special keyword (GCC used
            // to call it __import) or it can have a special attribute (GCC
            // currently marks it with [[__translated]]).
            //
            // Similarly, MSVC drops the `module;` marker and replaces all
            // other `module` keywords with `__preprocessed_module`.
            //
            // Clang doesn't appear to rewrite anything, at least as of
            // version 18.
            //
            if (bb == 0 && t.first)
            {
              const string& id (t.value); // Note: tracks t.

              // Handle the export prefix which can appear for both module
              // and import.
              //
              bool ex (false);
              if (id == "export")
              {
                if (l_->next (t) != type::identifier || t.first)
                {
                  n = false; // Could be module/import on next line.
                  continue;
                }

                ex = true;
                // Fall through.
              }

              if (id == "module" ||
                  (cid_->type == compiler_type::msvc &&
                   id == "__preprocessed_module"))
              {
                location_value l (get_location (t));
                l_->next (t);

                if ((t.type == type::semi     ||
                     t.type == type::identifier) && !t.first)
                  parse_module (t, ex, move (l));
                else
                  n = false;
              }
              else if (id == "import" /* ||
                       (cid_->type == compiler_type::gcc &&
                        id == "__import")*/)
              {
                l_->next (t);

                if ((t.type == type::less     ||
                     t.type == type::colon    ||
                     t.type == type::string   ||
                     t.type == type::identifier) && !t.first)
                  parse_import (t, ex);
                else
                  n = false;
              }
            }
            continue;
          }
        default:
          continue;
        }

        break;
      }

      // We used to issue an error here but the diagnostics and, especially,
      // the location are not very helpful. While the compilers don't do a
      // much better job at it, there are often other semantic errors that are
      // more helpful and which we cannot match. So now we warn and let the
      // compiler fail.
      //
      // Another option would have been to postpone this diagnostics until
      // after the compiler fails (and thus also confirming that it indeed has
      // failed) but that would require propagating this state from apply() to
      // perform_update() and also making sure we issue this diagnostics even
      // if anything in between fails (probably by having it sitting in a
      // diag_frame). So let's keep it simple for now.
      //
      // @@ We now do that for missing include, so could do here as well.
      //
      if (bb != 0)
        warn (t) << (bb > 0 ? "missing '}'" : "extraneous '}'");

      if (module_marker_ && u.module_info.name.empty ())
        fail (*module_marker_) << "module declaration expected after "
                               << "global module fragment";

      checksum = l.checksum ();
    }

    void parser::
    parse_module (token& t, bool ex, location_value l)
    {
      // enter: token after module keyword (l is the module keyword location)
      // leave: semi

      // Handle the leading 'module;' marker (p0713).
      //
      // Note that we don't bother diagnosing invalid/duplicate markers
      // leaving that to the compiler.
      //
      if (!ex && t.type == type::semi && !t.first)
      {
        module_marker_ = move (l);
        return;
      }

      // Otherwise it should be the start of the module name.
      //
      pair<string, bool> np (parse_module_name (t, true /* partition */));

      // Skip attributes (should be {}-balanced).
      //
      for (;
           t.type != type::eos && t.type != type::semi && !t.first;
           l_->next (t)) ;

      if (t.type != type::semi)
        fail (t) << "';' expected instead of " << t;
      else if (t.first)
        fail (t) << "';' must be on the same line";

      if (!u_->module_info.name.empty ())
        fail (l) << "multiple module declarations";

      u_->type = np.second
        ? (ex ? unit_type::module_intf_part : unit_type::module_impl_part)
        : (ex ? unit_type::module_intf      : unit_type::module_impl);
      u_->module_info.name = move (np.first);
    }

    void parser::
    parse_import (token& t, bool ex)
    {
      // enter: token after import keyword
      // leave: semi

      // Note that in import a partition can only be specified without a
      // module name. In other words, the following is invalid:
      //
      // module m;
      // import m:p;
      //
      string un;
      import_type ut;
      switch (t.type) // Start of module/header name.
      {
      case type::less:
      case type::string:
        {
          un = parse_header_name (t);
          ut = import_type::module_header;
          break;
        }
      case type::colon:
        {
          // Add the module name to the partition so that code that doesn't
          // need to distinguish between different kinds of imports doesn't
          // have to.
          //
          // Note that if this itself is a partition, then we need to strip
          // the partition part from the module name.
          //
          switch (u_->type)
          {
          case unit_type::module_intf:
          case unit_type::module_impl:
            un = u_->module_info.name;
            break;
          case unit_type::module_intf_part:
          case unit_type::module_impl_part:
            un.assign (u_->module_info.name, 0, u_->module_info.name.find (':'));
            break;
          default:
            fail (t) << "partition importation out of module purview";
          }

          parse_module_part (t, un);
          ut = import_type::module_part;
          break;
        }
      case type::identifier:
        {
          un = parse_module_name (t, false /* partition */).first;
          ut = import_type::module_intf;
          break;
        }
      default:
        assert (false);
        return;
      }

      // Skip attributes (should be {}-balanced).
      //
      for (;
           t.type != type::eos && t.type != type::semi && !t.first;
           l_->next (t)) ;

      if (t.type != type::semi)
        fail (t) << "';' expected instead of " << t;
      else if (t.first)
        fail (t) << "';' must be on the same line";

      // For now we skip header units (see a comment on module type/info
      // string serialization in compile rule for details). Note that
      // currently parse_header_name() always returns empty name.
      //
      if (ut == import_type::module_header)
        return;

      // Ignore duplicates. We don't expect a large numbers of (direct)
      // imports so vector/linear search is probably more efficient than a
      // set.
      //
      auto& is (u_->module_info.imports);

      auto i (find_if (is.begin (), is.end (),
                       [&un] (const module_import& i)
                       {
                         return i.name == un;
                       }));

      if (i == is.end ())
        is.push_back (module_import {ut, move (un), ex, 0});
      else
        i->exported = i->exported || ex;
    }

    pair<string, bool /* partition */> parser::
    parse_module_name (token& t, bool part)
    {
      // enter: first token of module name
      // leave: token after module name

      string n;

      // <identifier>[ . <identifier>]* [<module-part>]
      //
      for (;; l_->next (t))
      {
        if (t.type != type::identifier)
          fail (t) << "module name expected instead of " << t;
        else if (t.first)
          fail (t) << "module name must be on the same line";

        n += t.value;

        if (l_->next (t) != type::dot || t.first)
          break;

        n += '.';
      }

      bool p (part && t.type == type::colon && !t.first);
      if (p)
        parse_module_part (t, n);

      return make_pair (move (n), p);
    }

    void parser::
    parse_module_part (token& t, string& n)
    {
      // enter: colon
      // leave: token after module partition

      n += ':';

      // : <identifier>[ . <identifier>]*
      //
      for (;;)
      {
        if (l_->next (t) != type::identifier)
          fail (t) << "partition name expected instead of " << t;
        else if (t.first)
          fail (t) << "partition name must be on the same line";

        n += t.value;

        if (l_->next (t) != type::dot || t.first)
          break;

        n += '.';
      }
    }

    string parser::
    parse_header_name (token& t)
    {
      // enter: first token of module name, either string or less
      // leave: token after module name

      string n;

      // NOTE: actual name is a TODO if/when we need it.
      //
      if (t.type == type::string)
        /*n = move (t.value)*/;
      else
      {
        while (l_->next (t) != type::greater)
        {
          if (t.type == type::eos || t.first)
            fail (t) << "closing '>' expected after header name" << endf;
        }
      }

      l_->next (t);
      return n;
    }
  }
}
