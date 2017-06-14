// file      : build2/cc/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/parser.hxx>

#include <build2/cc/lexer.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using type = token_type;

    translation_unit parser::
    parse (istream& is, const path& name)
    {
      lexer l (is, name);
      l_ = &l;

      translation_unit u {"", false, {}};
      u_ = &u;

      // If the source has errors then we want the compiler to issues the
      // diagnostics. However, the errors could as likely be because we are
      // mis-parsing things. Initially, as a middle ground, we were going to
      // issue warnings. But the problem with this approach is that they are
      // easy to miss. So for now we fail.
      //
      size_t bb (0);     // {}-balance.
      bool   ex (false); // True if inside top-level export{} block.

      token t;
      for (bool n (true); (n ? l_->next (t) : t.type) != type::eos; )
      {
        // Break to stop, continue to continue, set n to false if the
        // next token already extracted.
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
            if (bb-- == 0)
              break; // Imbalance.

            if (ex && bb == 0)
              ex = false; // Closed top-level export{}.

            continue;
          }
        case type::identifier:
          {
            // Constructs we need to recognize (the last one is only not to
            // confuse it with others).
            //
            // [export]  import <module-name> [<attributes>] ;
            // [export]  module <module-name> [<attributes>] ;
            //  export { import <module-name> [<attributes>] ; }
            //  extern   module ...
            //
            const string& id (t.value);

            if (bb == 0)
            {
              if      (id == "import")
              {
                parse_import (t, false);
              }
              else if (id == "module")
              {
                parse_module (t, false);
              }
              else if (id == "export")
              {
                switch (l_->next (t))
                {
                case type::lcbrace:    ++bb; ex = true; break;
                case type::identifier:
                  {
                    if      (id == "module")
                      parse_module (t, true);
                    else if (id == "import")
                      parse_import (t, true);
                    else
                      n = false; // Something else (e.g., export namespace).

                    break;
                  }
                default: n = false; break;
                }
              }
              else if (id == "extern")
              {
                // Skip to make sure not recognized as module.
                //
                n = l_->next (t) == type::identifier && t.value == "module";
              }
            }
            else if (ex && bb == 1)
            {
              if (id == "import")
              {
                parse_import (t, true);
              }
            }
            continue;
          }
        default: continue;
        }

        break;
      }

      if (bb != 0)
        /*warn*/ fail (t) << "{}-imbalance detected";

      return u;
    }

    void parser::
    parse_import (token& t, bool ex)
    {
      // enter: import keyword
      // leave: semi

      l_->next (t); // Start of name.
      string n (parse_module_name (t));

      // Should be {}-balanced.
      //
      for (; t.type != type::eos && t.type != type::semi; l_->next (t)) ;

      if (t.type != type::semi)
        fail (t) << "';' expected instead of " << t;

      // Ignore duplicates. We don't expect a large numbers of imports so
      // vector/linear search is probably more efficient than a set.
      //
      auto& is (u_->module_imports);

      auto i (find_if (is.begin (), is.end (),
                       [&n] (const module_import& i)
                       {
                         return i.name == n;
                       }));

      if (i == is.end ())
        is.push_back (module_import {move (n), ex, 0});
      else
        i->exported = i->exported || ex;
    }

    void parser::
    parse_module (token& t, bool ex)
    {
      // enter: module keyword
      // leave: semi

      l_->next (t); // Start of name.
      string n (parse_module_name (t));

      // Should be {}-balanced.
      //
      for (; t.type != type::eos && t.type != type::semi; l_->next (t)) ;

      if (t.type != type::semi)
        fail (t) << "';' expected instead of " << t;

      if (!u_->module_name.empty ())
        fail (t) << "multiple module declarations";

      u_->module_name = move (n);
      u_->module_interface = ex;
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
        if (t.type != type::identifier)
          fail (t) << "module name expected instead of " << t;

        n += t.value;

        if (l_->next (t) != type::dot)
          break;

        n += '.';
      }

      return n;
    }
  }
}
