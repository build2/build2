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

    return make_pair (move (n), move (rp.second));
  }

  const target&
  to_target (const scope& s, name&& n, name&& o)
  {
    if (const target* r = search_existing (n, s, o.dir))
      return *r;

    fail << "target "
         << (n.pair ? names {move (n), move (o)} : names {move (n)})
         << " not found" << endf;
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
      fail << "unknown target type " << n.type << " in " << n;

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

      const target_type* ntt (to_target_type (s, n, p ? *++i : name ()).first);
      if (ntt == nullptr)
        fail << "unknown target type " << n.type << " in " << n;

      if ((find_if (tts.begin (), tts.end (),
                    [ntt] (const target_type* tt)
                    {
                      return ntt->is_a (*tt);
                    }) != tts.end ()) != out)
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
    // targets (see below).
    //
    function_family fn (m, "name");

    // Note that we must handle NULL values (relied upon by the parser
    // to provide conversion semantics consistent with untyped values).
    //
    fn["string"] += [](name* n)
    {
      return n != nullptr ? to_string (move (*n)) : string ();
    };

    fn["name"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).first.value;
    };
    fn["name"] += [](const scope* s, names ns)
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

    // Note: returns NULL if extension is unspecified (default) and empty if
    // specified as no extension.
    //
    fn["extension"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).second;
    };
    fn["extension"] += [](const scope* s, names ns)
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

    fn["directory"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).first.dir;
    };
    fn["directory"] += [](const scope* s, names ns)
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

    fn["target_type"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).first.type;
    };
    fn["target_type"] += [](const scope* s, names ns)
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

    // Note: returns NULL if no project specified.
    //
    fn["project"] += [](const scope* s, name n)
    {
      return to_target_name (s, move (n)).first.proj;
    };
    fn["project"] += [](const scope* s, names ns)
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
    fn["is_a"] += [](const scope* s, name n, names t)
    {
      return is_a (s, move (n), name (), move (t));
    };
    fn["is_a"] += [](const scope* s, names ns, names t)
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
    // Return names with target types which are-a (filter) or not are-a
    // (filter_out) one of <target-types>. See $is_a() for background.
    //
    fn["filter"] += [](const scope* s, names ns, names ts)
    {
      return filter (s, move (ns), move (ts), false /* out */);
    };

    fn["filter_out"] += [](const scope* s, names ns, names ts)
    {
      return filter (s, move (ns), move (ts), true /* out */);
    };

    // $size(<names>)
    //
    // Return the number of elements in the sequence.
    //
    fn["size"] += [] (names ns)
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

    // $sort(<names> [, <flags>])
    //
    // Sort names in ascending order.
    //
    // The following flags are supported:
    //
    //   dedup - in addition to sorting also remove duplicates
    //
    fn["sort"] += [] (names ns, optional<names> fs)
    {
      //@@ TODO: shouldn't we do this in a pair-aware manner?

      sort (ns.begin (), ns.end ());

      if (functions_sort_flags (move (fs)))
        ns.erase (unique (ns.begin(), ns.end()), ns.end ());

      return ns;
    };

    // $find(<names>, <name>)
    //
    // Return true if the name sequence contains the specified name.
    //
    fn["find"] += [](names vs, names v)
    {
      //@@ TODO: shouldn't we do this in a pair-aware manner?

      return find (vs.begin (), vs.end (),
                   convert<name> (move (v))) != vs.end ();
    };

    // $find_index(<names>, <name>)
    //
    // Return the index of the first element in the name sequence that is
    // equal to the specified name or $size(<names>) if none is found.
    //
    fn["find_index"] += [](names vs, names v)
    {
      //@@ TODO: shouldn't we do this in a pair-aware manner?

      auto i (find (vs.begin (), vs.end (), convert<name> (move (v))));
      return i != vs.end () ? i - vs.begin () : vs.size ();
    };

    // Functions that can be called only on real targets.
    //
    function_family ft (m, "target");

    // Note that while this function is not technically pure, we don't mark it
    // as such since it can only be called (normally form a recipe) after the
    // target has been matched, meaning that this target is a prerequisite and
    // therefore this impurity has been accounted for.
    //
    ft["path"] += [](const scope* s, names ns)
    {
      if (s == nullptr)
        fail << "target.path() called out of scope";

      // Most of the time we will have a single target so optimize for that.
      //
      small_vector<path, 1> r;

      for (auto i (ns.begin ()); i != ns.end (); ++i)
      {
        name& n (*i), o;
        const target& t (to_target (*s, move (n), move (n.pair ? *++i : o)));

        if (const auto* pt = t.is_a<path_target> ())
        {
          const path& p (pt->path ());

          if (&p != &empty_path)
            r.push_back (p);
          else
            fail << "target " << t << " path is not assigned";
        }
        else
          fail << "target " << t << " is not path-based";
      }

      // We want the result to be path if we were given a single target and
      // paths if multiple (or zero). The problem is, we cannot distinguish it
      // based on the argument type (e.g., name vs names) since passing an
      // out-qualified single target requires two names.
      //
      if (r.size () == 1)
        return value (move (r[0]));

      return value (paths (make_move_iterator (r.begin ()),
                           make_move_iterator (r.end ())));
    };

    // This one can only be called on a single target since we don't support
    // containers of process_path's (though we probably could).
    //
    // Note that while this function is not technically pure, we don't mark it
    // as such for the same reasons as $path() above.
    //
    ft["process_path"] += [](const scope* s, names ns)
    {
      if (s == nullptr)
        fail << "target.process_path() called out of scope";

      if (ns.empty () || ns.size () != (ns[0].pair ? 2 : 1))
        fail << "target.process_path() expects single target";

      name o;
      const target& t (
        to_target (*s, move (ns[0]), move (ns[0].pair ? ns[1] : o)));

      if (const auto* et = t.is_a<exe> ())
      {
        process_path r (et->process_path ());

        if (r.empty ())
          fail << "target " << t << " path is not assigned";

        return r;
      }
      else
        fail << "target " << t << " is not process_path-based" << endf;
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
