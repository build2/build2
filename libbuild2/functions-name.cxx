// file      : libbuild2/functions-name.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/functions-name.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>

using namespace std;

namespace build2
{
  extern bool
  functions_sort_flags (optional<names>); // functions-builtin.cxx

  // Convert name to target'ish name (see below for the 'ish part). Return
  // raw/unprocessed data in case this is an unknown target type (or called
  // out of scope). See scope::find_target_type() for details. Allow out-
  // qualified names (out is discarded).
  //
  static pair<const target_type*, optional<string>>
  to_target_type (const scope* s, name& n, const name& o = name ())
  {
    if (n.pair && !o.directory ())
      fail << "name pair in names";

    return s != nullptr
      ? s->find_target_type (n, location ())
      : pair<const target_type*, optional<string>> {nullptr, nullopt};
  }

  static pair<name, optional<string>>
  to_target_name (const scope* s, name&& n, const name& o = name ())
  {
    auto rp (to_target_type (s, n, o));

    if (rp.first != nullptr)
      n.type = rp.first->name;

    if (n.value.empty () && (n.type == "dir" || n.type == "fsdir"))
    {
      n.value = n.dir.leaf ().string ();
      n.dir.make_directory ();
    }

    return make_pair (move (n), move (rp.second));
  }

  const target&
  to_target (const scope& s, name&& n, name&& o)
  {
    // Note: help the user out and search in both out and src like a
    // prerequisite.
    //
    if (const target* r = search_existing (n, s, o.dir))
      return *r;

    // Inside recipes we don't treat `{}` as special so a literal target name
    // will have no type and won't be found, which is confusing as hell.
    //
    bool typed (n.typed ());

    diag_record dr (fail);

    dr << "target "
       << (n.pair ? names {move (n), move (o)} : names {move (n)})
       << " not found";

    if (!typed)
      dr << info << "wrap it in ([names] ...) if this is literal target name "
         << "specified inside recipe";

    dr << endf;
  }

  const target&
  to_target (const scope& s, names&& ns)
  {
    assert (ns.size () == (ns[0].pair ? 2 : 1));

    name o;
    return to_target (s, move (ns[0]), move (ns[0].pair ? ns[1] : o));
  }

  static bool
  is_a (const scope* s, name&& n, const name& o, names&& t)
  {
    if (s == nullptr)
      fail << "name.is_a() called out of scope";

    string tts (convert<string> (move (t)));
    const target_type* tt (s->find_target_type (tts));
    if (tt == nullptr)
      fail << "unknown target type " << tts;

    const target_type* ntt (to_target_type (s, n, o).first);
    if (ntt == nullptr)
    {
      // If this is an imported target and the target type is unknown, then
      // it cannot possibly match one of the known types. We handle it like
      // this instead of failing because the later failure (e.g., as a
      // result of this target listed as prerequisite) will have more
      // accurate diagnostics. See also filter() below.
      //
      if (n.proj)
        return false;

      fail << "unknown target type " << n.type << " in " << n;
    }

    return ntt->is_a (*tt);
  }

  static names
  filter (const scope* s, names ns, names ts, bool out)
  {
    if (s == nullptr)
      fail << "name." << (out ? "filter_out" : "filter")
           << "() called out of scope";

    small_vector<const target_type*, 1> tts;
    for (const name& n: ts)
    {
      if (!n.simple ())
        fail << "invalid target type name " << n;

      if (n.pair)
        fail << "pair in target type name " << n;

      const target_type* tt (s->find_target_type (n.value));
      if (tt == nullptr)
        fail << "unknown target type " << n.value;

      tts.push_back (tt);
    }

    names r;
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& n (*i);
      bool p (n.pair);

      // to_target_type() splits the name into the target name and extension.
      // While we could try to reconstitute it with combine_name(), there are
      // murky corner cases (see the default_extension argument) which won't
      // be easy to handle. So let's just make a copy. Looking at the
      // implementation of scope::find_target_type(), we can optimize for the
      // (common) typed case by only copying the type.
      //
      name c (n.typed () ? name (n.type, "") : n);

      const target_type* ntt (to_target_type (s, c, p ? *++i : name ()).first);
      if (ntt == nullptr)
      {
        // If this is an imported target and the target type is unknown, then
        // it cannot possibly match one of the known types. We handle it like
        // this instead of failing because the later failure (e.g., as a
        // result of this target listed as prerequisite) will have more
        // accurate diagnostics. See also is_a() above.
        //
        if (!n.proj)
          fail << "unknown target type " << n.type << " in " << n;
      }

      if (ntt != nullptr
          ? (find_if (tts.begin (), tts.end (),
                      [ntt] (const target_type* tt)
                      {
                        return ntt->is_a (*tt);
                      }) != tts.end ()) != out
          : out)
      {
        r.push_back (move (n));
        if (p)
          r.push_back (move (*i));
      }
    }

    return r;
  }

  void
  name_functions (function_map& m)
  {
    // These functions treat a name as a target/prerequisite name.
    //
    // While on one hand it feels like calling them target.name(), etc., would
    // have been more appropriate, on the other hand they can also be called
    // on prerequisite names. They also won't always return the same result as
    // if we were interrogating an actual target (e.g., the directory may be
    // relative). Plus we now have functions that can only be called on
    // targets (see functions-target.cxx).
    //
    function_family f (m, "name");

    // Note: let's leave this undocumented for now since it's not often needed
    // and is a can of worms.
    //
    // Note that we must handle NULL values (relied upon by the parser
    // to provide conversion semantics consistent with untyped values).
    //
    f["string"] += [](name* n)
    {
      return n != nullptr ? to_string (move (*n)) : string ();
    };

    // $name(<names>)
    //
    // Return the name of a target (or a list of names for a list of targets).
    //
    f["name"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).first.value;
    };
    f["name"] += [](const scope* s, names ns)
    {
      small_vector<string, 1> r;

      for (auto i (ns.begin ()); i != ns.end (); ++i)
      {
        name& n (*i);
        r.push_back (
          to_target_name (s, move (n), n.pair ? *++i : name ()).first.value);
      }

      if (r.size () == 1)
        return value (move (r[0]));

      return value (strings (make_move_iterator (r.begin ()),
                             make_move_iterator (r.end ())));
    };

    // $extension(<name>)
    //
    // Return the extension of a target.
    //
    // Note that this function returns `null` if the extension is unspecified
    // (default) and empty string if it's specified as no extension.
    //
    f["extension"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).second;
    };
    f["extension"] += [](const scope* s, names ns)
    {
      // Note: can't do multiple due to NULL semantics.
      //
      auto i (ns.begin ());

      name& n (*i);
      const name& o (n.pair ? *++i : name ());

      if (++i != ns.end ())
        fail << "invalid name value: multiple names"; // Like in convert().

      return to_target_name (s, move (n), o).second;
    };

    // $directory(<names>)
    //
    // Return the directory of a target (or a list of directories for a list
    // of targets).
    //
    f["directory"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).first.dir;
    };
    f["directory"] += [](const scope* s, names ns)
    {
      small_vector<dir_path, 1> r;

      for (auto i (ns.begin ()); i != ns.end (); ++i)
      {
        name& n (*i);
        r.push_back (
          to_target_name (s, move (n), n.pair ? *++i : name ()).first.dir);
      }

      if (r.size () == 1)
        return value (move (r[0]));

      return value (dir_paths (make_move_iterator (r.begin ()),
                               make_move_iterator (r.end ())));
    };

    // $target_type(<names>)
    //
    // Return the target type name of a target (or a list of target type names
    // for a list of targets).
    //
    f["target_type"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).first.type;
    };
    f["target_type"] += [](const scope* s, names ns)
    {
      small_vector<string, 1> r;

      for (auto i (ns.begin ()); i != ns.end (); ++i)
      {
        name& n (*i);
        r.push_back (
          to_target_name (s, move (n), n.pair ? *++i : name ()).first.type);
      }

      if (r.size () == 1)
        return value (move (r[0]));

      return value (strings (make_move_iterator (r.begin ()),
                             make_move_iterator (r.end ())));
    };

    // $project(<name>)
    //
    // Return the project of a target or `null` if not project-qualified.
    //
    f["project"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).first.proj;
    };
    f["project"] += [](const scope* s, names ns)
    {
      // Note: can't do multiple due to NULL semantics.
      //
      auto i (ns.begin ());

      name& n (*i);
      const name& o (n.pair ? *++i : name ());

      if (++i != ns.end ())
        fail << "invalid name value: multiple names"; // Like in convert().

      return to_target_name (s, move (n), o).first.proj;
    };

    // $is_a(<name>, <target-type>)
    //
    // Return true if the <name>'s target type is-a <target-type>. Note that
    // this is a dynamic type check that takes into account target type
    // inheritance.
    //
    f["is_a"] += [](const scope* s, name n, names t)
    {
      return is_a (s, move (n), name (), move (t));
    };
    f["is_a"] += [](const scope* s, names ns, names t)
    {
      auto i (ns.begin ());

      name& n (*i);
      const name& o (n.pair ? *++i : name ());

      if (++i != ns.end ())
        fail << "invalid name value: multiple names"; // Like in convert().

      return is_a (s, move (n), o, move (t));
    };

    // $filter(<names>, <target-types>)
    // $filter_out(<names>, <target-types>)
    //
    // Return names with target types which are-a (`filter`) or not are-a
    // (`filter_out`) one of <target-types>. See `$is_a()` for background.
    //
    f["filter"] += [](const scope* s, names ns, names ts)
    {
      return filter (s, move (ns), move (ts), false /* out */);
    };

    f["filter_out"] += [](const scope* s, names ns, names ts)
    {
      return filter (s, move (ns), move (ts), true /* out */);
    };

    // $size(<names>)
    //
    // Return the number of elements in the sequence.
    //
    f["size"] += [] (names ns)
    {
      size_t n (0);

      for (auto i (ns.begin ()); i != ns.end (); ++i)
      {
        ++n;
        if (i->pair && !(++i)->directory ())
          fail << "name pair in names";
      }

      return n;
    };

    // $sort(<names>[, <flags>])
    //
    // Sort names in ascending order.
    //
    // The following flags are supported:
    //
    //     dedup - in addition to sorting also remove duplicates
    //
    f["sort"] += [] (names ns, optional<names> fs)
    {
      //@@ TODO: shouldn't we do this in a pair-aware manner?

      sort (ns.begin (), ns.end ());

      if (functions_sort_flags (move (fs)))
        ns.erase (unique (ns.begin (), ns.end ()), ns.end ());

      return ns;
    };

    // $find(<names>, <name>)
    //
    // Return true if the name sequence contains the specified name.
    //
    f["find"] += [](names vs, names v)
    {
      //@@ TODO: shouldn't we do this in a pair-aware manner?

      return find (vs.begin (), vs.end (),
                   convert<name> (move (v))) != vs.end ();
    };

    // $find_index(<names>, <name>)
    //
    // Return the index of the first element in the name sequence that is
    // equal to the specified name or `$size(names)` if none is found.
    //
    f["find_index"] += [](names vs, names v)
    {
      //@@ TODO: shouldn't we do this in a pair-aware manner?

      auto i (find (vs.begin (), vs.end (), convert<name> (move (v))));
      return i != vs.end () ? i - vs.begin () : vs.size ();
    };

    // Name-specific overloads from builtins.
    //
    function_family fb (m, "builtin");

    // Note that while we should normally handle NULL values (relied upon by
    // the parser to provide concatenation semantics consistent with untyped
    // values), the result will unlikely be what the user expected. So for now
    // we keep it a bit tighter.
    //
    fb[".concat"] += [](dir_path d, name n)
    {
      d /= n.dir;
      n.dir = move (d);
      return n;
    };
  }
}
