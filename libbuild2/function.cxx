// file      : libbuild2/function.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>

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
    // See the overall function machinery description for the ranking
    // semantics.
    //
    const function_overloads* all_ovls (find (name));

    size_t rank (~0);
    small_vector<const function_overload*, 2> ovls;
    if (all_ovls != nullptr)
    {
      size_t count (args.size ());

      for (auto it (all_ovls->begin ()); it != all_ovls->end (); ++it)
      {
        const function_overload& f (*it);

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
        auto df = make_diag_frame (
          [fa, &loc, &print_call] (const diag_record& dr)
          {
            if (fa)
            {
              dr << info (loc) << "while calling ";
              print_call (dr.os);
            }
          });

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
              untypify (args[i], true /* reduce */);
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

        if (all_ovls != nullptr)
        {
          for (auto i (all_ovls->begin ()); i != all_ovls->end (); ++i)
            dr << info << "candidate: " << *i;
        }

        // If this is an unqualified name, then also print qualified
        // functions that end with this name. But skip functions that we
        // have already printed in the previous loop.
        //
        if (name.find ('.') == string::npos)
        {
          size_t n (name.size ());

          for (auto i (begin ()); i != end (); ++i)
          {
            const string& q (i->first);

            if (q.size () > n)
            {
              for (auto j (i->second.begin ()); j != i->second.end (); ++j)
              {
                const function_overload& f (*j);

                if (f.alt_name == nullptr || f.alt_name != name)
                {
                  size_t p (q.size () - n);
                  if (q[p - 1] == '.' && q.compare (p, n, name) == 0)
                    dr << info << "candidate: " << f;
                }
              }
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

  auto function_family::
  insert (string n, bool pure) const -> entry
  {
    // Figure out qualification.
    //
    string qn;
    size_t p (n.find ('.'));

    if (p == string::npos)
    {
      if (!qual_.empty ())
      {
        qn = qual_;
        qn += '.';
        qn += n;
      }
    }
    else if (p == 0)
    {
      assert (!qual_.empty ());
      n.insert (0, qual_);
    }

    return entry {
      map_.insert (move (n), pure),
      qn.empty () ? nullptr : &map_.insert (move (qn), pure),
      thunk_};
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

#if !defined(_WIN32)
  constexpr const optional<const value_type*>* function_args<>::types;
#else
  const optional<const value_type*>* const function_args<>::types = nullptr;
#endif

  // Static-initialize the function map and populate with builtin functions.
  //
  // NOTE: remember to also arrange for automatic documentation extraction in
  //       doc/buildfile!

  void bool_functions (function_map&);           // functions-bool.cxx
  void builtin_functions (function_map&);        // functions-builtin.cxx
  void filesystem_functions (function_map&);     // functions-filesystem.cxx
  void integer_functions (function_map&);        // functions-integer.cxx
  void json_functions (function_map&);           // functions-json.cxx
  void name_functions (function_map&);           // functions-name.cxx
  void path_functions (function_map&);           // functions-path.cxx
  void process_functions (function_map&);        // functions-process.cxx
  void process_path_functions (function_map&);   // functions-process-path.cxx
  void regex_functions (function_map&);          // functions-regex.cxx
  void string_functions (function_map&);         // functions-string.cxx
  void target_functions (function_map&);         // functions-target.cxx
  void target_triplet_functions (function_map&); // functions-target-triplet.cxx
  void project_name_functions (function_map&);   // functions-target-triplet.cxx


  void
  insert_builtin_functions (function_map& m)
  {
    bool_functions (m);
    builtin_functions (m);
    filesystem_functions (m);
    integer_functions (m);
    json_functions (m);
    name_functions (m);
    path_functions (m);
    process_functions (m);
    process_path_functions (m);
    regex_functions (m);
    string_functions (m);
    target_functions (m);
    target_triplet_functions (m);
    project_name_functions (m);
  }
}
