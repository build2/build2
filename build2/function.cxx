// file      : build2/function.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function>

#include <cstring> // strchr()

using namespace std;

namespace build2
{
  ostream&
  operator<< (ostream& os, const function_overload& f)
  {
    os << f.name << '(';

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

    if (f.alt_name != nullptr)
    {
      auto k (strchr (f.alt_name, '.') == nullptr
              ? "unqualified"
              : "qualified");

      os << ", " << k << " name " << f.alt_name;
    }

    return os;
  }

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

  pair<value, bool> function_map::
  call (const string& name,
        vector_view<value> args,
        const location& loc,
        bool fa) const
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

    // Overload resolution.
    //
    // Ours is pretty simple: if all the arguments match exactly, then we have
    // a perfect match. Otherwise, if any argument matches via the derived-to-
    // base conversion, then we have an imperfect match. More than one perfect
    // or imperfect match is ambiguous (i.e., we don't try to rank imperfect
    // matches).
    //
    size_t count (args.size ());
    auto ip (map_.equal_range (name));

    // First look for a perfect match, then for imperfect. We do it this way
    // to make sure we always stay small in the successful case.
    //
    small_vector<const function_overload*, 1> r;

    for (bool perf (true);; perf = false)
    {
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
          {
            if (!f.arg_types[i]) // Anytyped.
              continue;

            const value_type* at (args[i].type);
            const value_type* ft (*f.arg_types[i]);

            if (at == ft) // Types match perfectly.
              continue;

            if (!perf && at != nullptr && ft != nullptr)
            {
              while ((at = at->base) != nullptr && at != ft) ;

              if (at != nullptr) // Types match via derived-to-base.
                continue;
            }

            break;
          }

          if (i != n)
            continue;
        }

        r.push_back (&f); // Continue looking to detect ambiguities.
      }

      if (!r.empty () || !perf)
        break;
    }

    switch (r.size ())
    {
    case 1:
      {
        // Print the call location in case the function fails.
        //
        auto g (
          make_exception_guard (
            [fa, &loc, &print_call] ()
            {
              if (fa && verb != 0)
              {
                diag_record dr (info (loc));
                dr << "while calling "; print_call (dr.os);
              }
            }));

        auto f (r.back ());
        return make_pair (f->impl (move (args), *f), true);
      }
    case 0:
      {
        if (!fa)
          return make_pair (value (nullptr), false);

        // No match.
        //
        diag_record dr;

        dr << fail (loc) << "unmatched call to "; print_call (dr.os);

        for (auto i (ip.first); i != ip.second; ++i)
          dr << info << "candidate: " << i->second;

        // If this is an unqualified name, then also print qualified
        // functions that end with this name. But skip functions that we
        // have already printed in the previous loop.
        //
        if (name.find ('.') == string::npos)
        {
          size_t n (name.size ());

          for (auto i (functions.begin ()); i != functions.end (); ++i)
          {
            const string& q (i->first);
            const function_overload& f (i->second);

            if ((f.alt_name == nullptr || f.alt_name != name) &&
                q.size () > n)
            {
              size_t p (q.size () - n);
              if (q[p - 1] == '.' && q.compare (p, n, name) == 0)
                dr << info << "candidate: " << i->second;
            }
          }
        }

        dr << endf;
      }
    default:
      {
        // Ambigous match.
        //
        diag_record dr;
        dr << fail (loc) << "ambiguous call to "; print_call (dr.os);

        for (auto f: r)
          dr << info << "candidate: " << *f;

        dr << endf;
      }
    }
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
    diag_record dr (fail);
    dr << "invalid argument";

    const char* w (e.what ());
    if (*w != '\0')
      dr << ": " << w;

    dr << endf;
  }

#if !defined(_MSC_VER) || _MSC_VER > 1910
  constexpr const optional<const value_type*>* function_args<>::types;
#else
  const optional<const value_type*>* const function_args<>::types = nullptr;
#endif

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

    auto i (qn.empty () ? functions.end () : functions.insert (move (qn), f));
    auto j (functions.insert (move (n), move (f)));

    // If we have both, then set alternative names.
    //
    if (i != functions.end ())
    {
      i->second.alt_name = j->first.c_str ();
      j->second.alt_name = i->first.c_str ();
    }
  }

  // Static-initialize the function map and populate with builtin functions.
  //
  function_map functions;

  void builtin_functions ();       // functions-builtin.cxx
  void path_functions ();          // functions-path.cxx
  void process_path_functions ();  // functions-process-path.cxx
  void string_functions ();        // functions-string.cxx

  struct functions_init
  {
    functions_init ()
    {
      builtin_functions ();
      path_functions ();
      process_path_functions ();
      string_functions ();
    }
  };

  static const functions_init init_;
}
