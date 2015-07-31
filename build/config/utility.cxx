// file      : build/config/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/config/utility>

using namespace std;

namespace build
{
  namespace config
  {
    // The same as the template except it is a bit more efficient
    // when it comes to not creating the default value string
    // unnecessarily.
    //
    pair<const string&, bool>
    required (scope& root, const char* name, const char* def_value)
    {
      string r;
      const variable& var (variable_pool.find (name));

      if (auto v = root[var])
      {
        const string& s (v.as<const string&> ());

        if (!v.belongs (*global_scope)) // A value from (some) config.build.
          return pair<const string&, bool> (s, false);

        r = s;
      }
      else
        r = def_value;

      auto v (root.assign (var));
      v = move (r);

      return pair<const string&, bool> (v.as<const string&> (), true);
    }

    bool
    specified (scope& r, const string& ns)
    {
      // Search all outer scopes for any value in this namespace.
      //
      for (scope* s (&r); s != nullptr; s = s->parent_scope ())
      {
        auto p (s->vars.find_namespace (ns));
        if (p.first != p.second)
          return true;
      }

      return false;
    }

    void
    append_options (cstrings& args, const list_value& lv, const char* var)
    {
      for (const name& n: lv)
      {
        if (n.simple ())
          args.push_back (n.value.c_str ());
        else if (n.directory ())
          args.push_back (n.dir.string ().c_str ());
        else
          fail << "expected option instead of " << n <<
            info << "in variable " << var;
      }
    }
  }
}
