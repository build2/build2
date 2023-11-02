// file      : libbuild2/functions-builtin.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <sstream>

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  // Note: not static since used by type-specific sort() implementations.
  //
  bool
  functions_sort_flags (optional<names> fs)
  {
    bool r (false);
    if (fs)
    {
      for (name& f: *fs)
      {
        string s (convert<string> (move (f)));

        if (s == "dedup")
          r = true;
        else
          throw invalid_argument ("invalid flag '" + s + '\'');
      }
    }
    return r;
  };

  void
  builtin_functions (function_map& m)
  {
    function_family f (m, "builtin");

    // $defined(<variable>)
    //
    // Return true if the specified variable is defined in the calling scope or
    // any outer scopes.
    //
    // Note that this function is not pure.
    //

    // Note that we may want to extend the scope argument to a more general
    // notion of "lookup context" (scope, target, prerequisite).
    //
    f.insert ("defined", false) += [](const scope* s, names name)
    {
      if (s == nullptr)
        fail << "defined() called out of scope" << endf;

      return (*s)[convert<string> (move (name))].defined ();
    };

    // $visibility(<variable>)
    //
    // Return variable visibility if it is known and `null` otherwise.
    //
    // Possible visibility value are:
    //
    //     global  -- all outer scopes
    //     project -- this project (no outer projects)
    //     scope   -- this scope (no outer scopes)
    //     target  -- target and target type/pattern-specific
    //     prereq  -- prerequisite-specific
    //
    // Note that this function is not pure.
    //
    f.insert ("visibility", false) += [](const scope* s, names name)
    {
      if (s == nullptr)
        fail << "visibility() called out of scope" << endf;

      const variable* var (
        s->var_pool ().find (convert<string> (move (name))));

      return (var != nullptr
              ? optional<string> (to_string (var->visibility))
              : nullopt);
    };

    // $type(<value>)
    //
    // Return the type name of the value or empty string if untyped.
    //
    f["type"] += [](value* v) {return v->type != nullptr ? v->type->name : "";};

    // $null(<value>)
    //
    // Return true if the value is `null`.
    //
    f["null"] += [](value* v) {return v->null;};

    // $empty(<value>)
    //
    // Return true if the value is empty.
    //
    f["empty"] += [](value* v)  {return v->null || v->empty ();};


    // $first(<value>[, <not_pair>])
    // $second(<value>[, <not_pair>])
    //
    // Return the first or the second half of a pair, respectively. If a value
    // is not a pair, then return `null` unless the <not_pair> argument is
    // `true`, in which case return the non-pair value.
    //
    // If multiple pairs are specified, then return the list of first/second
    // halfs. If an element is not a pair, then omit it from the resulting
    // list unless the <not_pair> argument is `true`, in which case add the
    // non-pair element to the list.
    //
    f["first"] += [] (names ns, optional<value> not_pair)
    {
      // @@ TODO: would be nice to return typed half if passed typed value.

      bool np (not_pair && convert<bool> (move (*not_pair)));

      names r;
      for (auto i (ns.begin ()), e (ns.end ()); i != e; )
      {
        name& f (*i++);
        name* s (f.pair ? &*i++ : nullptr);

        if (s != nullptr || np)
        {
          f.pair = '\0';
          r.push_back (move (f));
        }
        else if (ns.size () == 1)
          return value (nullptr); // Single non-pair.
      }

      return value (move (r));
    };

    f["second"] += [] (names ns, optional<value> not_pair)
    {
      bool np (not_pair && convert<bool> (move (*not_pair)));

      names r;
      for (auto i (ns.begin ()), e (ns.end ()); i != e; )
      {
        name& f (*i++);
        name* s (f.pair ? &*i++ : nullptr);

        if (s != nullptr)
          r.push_back (move (*s));
        else if (np)
          r.push_back (move (f));
        else if (ns.size () == 1)
          return value (nullptr); // Single non-pair.
      }

      return value (move (r));
    };

    // Leave this one undocumented for now since it's unclear why would anyone
    // want to use it currently (we don't yet have any function composition
    // facilities).
    //
    f["identity"] += [](value* v) {return move (*v);};

    // $quote(<value>[, <escape>])
    //
    // Quote the value returning its string representation. If <escape> is
    // `true`, then also escape (with a backslash) the quote characters being
    // added (this is useful if the result will be re-parsed, for example as a
    // script command line).
    //
    f["quote"] += [](value* v, optional<value> escape)
    {
      if (v->null)
        return string ();

      untypify (*v, true /* reduce */); // Reverse to names.

      ostringstream os;
      to_stream (os,
                 v->as<names> (),
                 quote_mode::normal,
                 '@'  /* pair */,
                 escape && convert<bool> (move (*escape)));
      return os.str ();
    };

    // $getenv(<name>)
    //
    // Get the value of the environment variable. Return `null` if the
    // environment variable is not set.
    //
    // Note that if the build result can be affected by the variable being
    // queried, then it should be reported with the `config.environment`
    // directive.
    //
    // Note that this function is not pure.
    //
    f.insert ("getenv", false) += [](names name)
    {
      optional<string> v (getenv (convert<string> (move (name))));

      if (!v)
        return value ();

      names r;
      r.emplace_back (to_name (move (*v)));
      return value (move (r));
    };
  }
}
