// file      : build2/function.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function.hxx>

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

  bool function_map::
  defined (const string& name) const
  {
    assert (!name.empty ());

    // If this is a qualified function name then check if it is already
    // defined.
    //
    if (name.back () != '.')
      return map_.find (name) != map_.end ();

    // If any function of the specified family is already defined, then one of
    // them should be the first element that is greater than the dot-terminated
    // family name. Here we rely on the fact that the dot character is less
    // than any character of unqualified function and family names.
    //
    size_t n (name.size ());
    assert (n > 1);

    auto i (map_.upper_bound (name));
    return i != map_.end () && i->first.compare (0, n, name) == 0;
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
  call (const scope* base,
        const string& name,
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
    // Ours is pretty simple: we sort all the overloads into three ranks:
    //
    // 0 -- all the arguments match exactly (perfect match)
    // 1 -- one or more arguments match via the derived-to-base conversion
    // 2 -- one or more arguments match via the reversal to untyped
    //
    // More than one match of the same rank is ambiguous.
    //
    auto ip (map_.equal_range (name));

    size_t rank (~0);
    small_vector<const function_overload*, 2> ovls;
    {
      size_t count (args.size ());

      for (auto it (ip.first); it != ip.second; ++it)
      {
        const function_overload& f (it->second);

        // Argument count match.
        //
        if (count < f.arg_min || count > f.arg_max)
          continue;

        // Argument types match.
        //
        size_t r (0);
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

            if (at != nullptr && ft != nullptr)
            {
              while ((at = at->base_type) != nullptr && at != ft) ;

              if (at != nullptr) // Types match via derived-to-base.
              {
                if (r < 1)
                  r = 1;
                continue;
              }
            }

            if (ft == nullptr) // Types match via reversal to untyped.
            {
              if (r < 2)
                r = 2;
              continue;
            }

            break; // No match.
          }

          if (i != n)
            continue; // No match.
        }

        // Better or just as good a match?
        //
        if (r <= rank)
        {
          if (r < rank) // Better.
          {
            rank = r;
            ovls.clear ();
          }

          ovls.push_back (&f);
        }

        // Continue looking to detect ambiguities.
      }
    }

    switch (ovls.size ())
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

        auto f (ovls.back ());

        // If one or more arguments match via the reversal to untyped (rank 2),
        // then we need to go over the overload's arguments one more time an
        // untypify() those that we need to reverse.
        //
        if (rank == 2)
        {
          size_t n (args.size ());
          assert (n <= f->arg_types.size ());

          for (size_t i (0); i != n; ++i)
          {
            if (f->arg_types[i]             &&
                *f->arg_types[i] == nullptr &&
                args[i].type != nullptr)
              untypify (args[i]);
          }
        }

        try
        {
          return make_pair (f->impl (base, move (args), *f), true);
        }
        catch (const invalid_argument& e)
        {
          diag_record dr (fail);
          dr << "invalid argument";

          if (*e.what () != '\0')
            dr << ": " << e;

          dr << endf;
        }
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

        for (auto f: ovls)
          dr << info << "candidate: " << *f;

        dr << endf;
      }
    }
  }

  value function_family::
  default_thunk (const scope* base,
                 vector_view<value> args,
                 const function_overload& f)
  {
    // Call the cast thunk.
    //
    struct cast_data // Prefix of function_cast::data.
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
    };

    auto d (reinterpret_cast<const cast_data*> (&f.data));
    return d->thunk (base, move (args), d);
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

  void builtin_functions ();        // functions-builtin.cxx
  void filesystem_functions ();     // functions-filesystem.cxx
  void name_functions ();           // functions-name.cxx
  void path_functions ();           // functions-path.cxx
  void process_functions ();        // functions-process.cxx
  void process_path_functions ();   // functions-process-path.cxx
  void regex_functions ();          // functions-regex.cxx
  void string_functions ();         // functions-string.cxx
  void target_triplet_functions (); // functions-target-triplet.cxx
  void project_name_functions ();   // functions-target-triplet.cxx

  struct functions_init
  {
    functions_init ()
    {
      builtin_functions ();
      filesystem_functions ();
      name_functions ();
      path_functions ();
      process_functions ();
      process_path_functions ();
      regex_functions ();
      string_functions ();
      target_triplet_functions ();
      project_name_functions ();
    }
  };

  static const functions_init init_;
}
