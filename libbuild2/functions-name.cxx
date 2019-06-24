// file      : libbuild2/functions-name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  // Convert name to target'ish name (see below for the 'ish part). Return
  // raw/unprocessed data in case this is an unknown target type (or called
  // out of scope). See scope::find_target_type() for details.
  //
  static pair<name, optional<string>>
  to_target (const scope* s, name&& n)
  {
    optional<string> e;

    if (s != nullptr)
    {
      auto rp (s->find_target_type (n, location ()));

      if (rp.first != nullptr)
        n.type = rp.first->name;

      e = move (rp.second);
    }

    return make_pair (move (n), move (e));
  }

  void
  name_functions ()
  {
    function_family f ("name");

    // These functions treat a name as a target/prerequisite name.
    //
    // While on one hand it feels like calling them target.name(), etc., would
    // have been more appropriate, on the other hand they can also be called
    // on prerequisite names. They also won't always return the same result as
    // if we were interrogating an actual target (e.g., the directory may be
    // relative).
    //
    f["name"] = [](const scope* s, name n)
    {
      return to_target (s, move (n)).first.value;
    };
    f["name"] = [](const scope* s, names ns)
    {
      return to_target (s, convert<name> (move (ns))).first.value;
    };

    // Note: returns NULL if extension is unspecified (default) and empty if
    // specified as no extension.
    //
    f["extension"] = [](const scope* s, name n)
    {
      return to_target (s, move (n)).second;
    };
    f["extension"] = [](const scope* s, names ns)
    {
      return to_target (s, convert<name> (move (ns))).second;
    };

    f["directory"] = [](const scope* s, name n)
    {
      return to_target (s, move (n)).first.dir;
    };
    f["directory"] = [](const scope* s, names ns)
    {
      return to_target (s, convert<name> (move (ns))).first.dir;
    };

    f["target_type"] = [](const scope* s, name n)
    {
      return to_target (s, move (n)).first.type;
    };
    f["target_type"] = [](const scope* s, names ns)
    {
      return to_target (s, convert<name> (move (ns))).first.type;
    };

    // Note: returns NULL if no project specified.
    //
    f["project"] = [](const scope* s, name n)
    {
      return to_target (s, move (n)).first.proj;
    };
    f["project"] = [](const scope* s, names ns)
    {
      return to_target (s, convert<name> (move (ns))).first.proj;
    };

    // Name-specific overloads from builtins.
    //
    function_family b ("builtin");

    b[".concat"] = [](dir_path d, name n)
    {
      d /= n.dir;
      n.dir = move (d);
      return n;
    };
  }
}
