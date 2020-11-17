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

    unit parser::
    parse (ifdstream& is, const path_name& in)
    {
      lexer l (is, in);
      l_ = &l;

      unit u;
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
            //           module                              ;
            // [export]  module <module-name> [<attributes>] ;
            // [export]  import <module-name> [<attributes>] ;
            // [export]  import <header-name> [<attributes>] ;
            //
            // The leading module/export/import keyword should be the first
            // token of a logical line and only if certain characters appear
            // after module/import and all the tokens are on the same line,
            // then the line is recognized as a pseudo-directive; see p1857
            // for details.
            //
            // Additionally, when include is translated to an import, it's
            // normally replaced with the special __import keyword since it
            // may appear in C context.
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

              if (id == "module")
              {
                location_value l (get_location (t));
                l_->next (t);

                if ((t.type == type::semi     ||
                     t.type == type::identifier) && !t.first)
                  parse_module (t, ex, move (l));
                else
                  n = false;
              }
              else if (id == "import" || id == "__import")
              {
                l_->next (t);

                if ((t.type == type::less     ||
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
                               << "leading module marker";

      checksum = l.checksum ();
      return u;
    }

    void parser::
    parse_import (token& t, bool ex)
    {
      // enter: token after import keyword
      // leave: semi

      string un;
      unit_type ut;
      switch (t.type) // Start of module/header name.
      {
      case type::less:
      case type::string:
        {
          un = parse_header_name (t);
          ut = unit_type::module_header;
          break;
        }
      case type::identifier:
        {
          un = parse_module_name (t);
          ut = unit_type::module_iface;
          break;
        }
      default:
        assert (false);
      }

      // Should be {}-balanced.
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
      if (ut == unit_type::module_header)
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
      string n (parse_module_name (t));

      // Should be {}-balanced.
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

      u_->type = ex ? unit_type::module_iface : unit_type::module_impl;
      u_->module_info.name = move (n);
    }

    string parser::
    parse_module_name (token& t)
    {
      // enter: first token of module name
      // leave: token after module name

      string n;

      // <identifier>[ . <identifier>]*
      //
      for (;; l_->next (t))
      {
        if (t.type != type::identifier || t.first)
          fail (t) << "module name expected instead of " << t;

        n += t.value;

        if (l_->next (t) != type::dot || t.first)
          break;

        n += '.';
      }

      return n;
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
