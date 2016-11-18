// file      : build2/function.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function>

using namespace std;

namespace build2
{
  auto function_map::
  insert (string name, function_overload f) -> iterator
  {
    // Sanity checks.
    //
    assert (f.arg_min <= f.arg_max &&
            f.arg_types.size () <= f.arg_max &&
            f.impl != nullptr);

    auto i (map_.emplace (move (name), move (f)));

    i->second.name = i->first.c_str ();
    return i;
  }

  value function_map::
  call (const string& name, vector_view<value> args, const location& loc)
  {
    auto print_call = [&name, &args] (ostream& os)
    {
      os << name << '(';

      for (size_t i (0); i != args.size (); ++i)
      {
        const value_type* t (args[i].type);
        os << (i != 0 ? ", " : "") << (t != nullptr ? t->name : "<untyped>");
      }

      os << ')';
    };

    auto print_fovl = [&name] (ostream& os, const function_overload& f)
    {
      os << name << '(';

      bool v (f.arg_max == function_overload::arg_variadic);
      size_t n (v ? max (f.arg_min, f.arg_types.size ()): f.arg_max);

      // Handle variadic tail as the last pseudo-argument.
      //
      for (size_t i (0); i != n + (v ? 1 : 0); ++i)
      {
        if (i == f.arg_min)
          os << (i != 0 ? " [" : "[");

        os << (i != 0 ? ", " : "");

        if (i == n) // Variadic tail (last).
          os << "...";
        else
        {
          // If count is greater than f.arg_typed, then we assume the rest are
          // valid but untyped.
          //
          const optional<const value_type*> t (
            i < f.arg_types.size () ? f.arg_types[i] : nullopt);

          os << (t ? (*t != nullptr ? (*t)->name : "<untyped>") : "<anytype>");
        }
      }

      if (n + (v ? 1 : 0) > f.arg_min)
        os << ']';

      os << ')';

      if (f.qual_name)
        os << ", qualified name " << f.qual_name;
    };

    // Overload resolution.
    //
    const function_overload* r (nullptr);

    size_t count (args.size ());
    auto ip (map_.equal_range (name));

    for (auto it (ip.first); it != ip.second; ++it)
    {
      const function_overload& f (it->second);

      // Argument count match.
      //
      if (count < f.arg_min || count > f.arg_max)
        continue;

      // Argument types match.
      //
      {
        size_t i (0), n (min (count, f.arg_types.size ()));
        for (; i != n; ++i)
          if (f.arg_types[i] && *f.arg_types[i] != args[i].type)
            break;

        if (i != n)
          continue;
      }

      if (r != nullptr)
      {
        diag_record dr (fail (loc));

        dr << "ambiguous call to "; print_call (dr.os);
        dr << info << " candidate: "; print_fovl (dr.os, *r);
        dr << info << " candidate: "; print_fovl (dr.os, f);
      }

      r = &f; // Continue looking to detect ambiguities.
    }

    if (r == nullptr)
    {
      diag_record dr (fail (loc));

      dr << "unmatched call to "; print_call (dr.os);

      for (auto it (ip.first); it != ip.second; ++it)
      {
        const function_overload& f (it->second);

        dr << info << " candidate: "; print_fovl (dr.os, f);
      }
    }

    // Print the call location if the function fails.
    //
    auto g (
      make_exception_guard (
        [&loc, &print_call] ()
        {
          if (verb != 0)
          {
            diag_record dr (info (loc));
            dr << "while calling "; print_call (dr.os);
          }
        }));

    return r->impl (move (args), *r);
  }

  value function_family::
  default_thunk (vector_view<value> args, const function_overload& f)
  try
  {
    // Call the cast thunk.
    //
    struct cast_data // Prefix of function_cast::data.
    {
      value (*const thunk) (vector_view<value>, const void*);
    };

    auto d (reinterpret_cast<const cast_data*> (&f.data));
    return d->thunk (move (args), d);
  }
  catch (const invalid_argument& e)
  {
    {
      diag_record dr (error);
      dr << "invalid argument";

      const char* w (e.what ());
      if (*w != '\0')
        dr << ": " << w;
    }

    throw failed ();
  }

  const optional<const value_type*>* const function_args<>::types = nullptr;

  void function_family::entry::
  insert (string n, function_overload f) const
  {
    // Figure out qualification.
    //
    string qn;
    size_t p (n.find ('.'));

    if (p == string::npos)
    {
      if (!qual.empty ())
      {
        qn = qual;
        qn += '.';
        qn += n;
      }
    }
    else if (p == 0)
    {
      assert (!qual.empty ());
      n.insert (0, qual);
    }

    // First insert the qualified name and use its key for f.qual_name.
    //
    if (!qn.empty ())
    {
      auto i (functions.insert (move (qn), f));
      f.qual_name = i->first.c_str ();
    }

    functions.insert (move (n), move (f));
  }

  // Static-initialize the function map and populate with builtin functions.
  //
  function_map functions;

  void
  path_functions (); // functions-path.cxx

  struct functions_init
  {
    functions_init ()
    {
      path_functions ();
    }
  };

  static const functions_init init_;
}
