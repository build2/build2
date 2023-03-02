// file      : libbuild2/function.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_FUNCTION_HXX
#define LIBBUILD2_FUNCTION_HXX

#include <cstddef>      // max_align_t
#include <utility>      // index_sequence
#include <type_traits>  // is_*

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Functions can be overloaded based on types of their arguments but
  // arguments can be untyped and a function can elect to accept an argument
  // of any type.
  //
  // Functions can be qualified (e.g, string.length(), path.directory()) and
  // unqualified (e.g., length(), directory()). Only functions overloaded on
  // static types can be unqualified plus they should also define a qualified
  // alias.
  //
  // Low-level function implementation would be called with a list of values
  // as arguments. There is also higher-level, more convenient support for
  // defining functions as pointers to functions (including capture-less
  // lambdas), pointers to member functions (e.g., string::size()), or
  // pointers to data members (e.g., name::type). In this case the buildfile
  // function types are automatically matched to C++ function types according
  // to these rules:
  //
  // T           - statically-typed (value_traits<T> must be defined)
  // names       - untyped
  // value       - any type
  // T*          - NULL-able argument (here T can be names)
  // value*      - NULL-able any type (never NULL itself, use value::null)
  // optional<T> - optional argument (here T can be T*, names, value)
  //
  // The overload resolution is pretty simple: we sort all the candidates into
  // three ranks:
  //
  // 0 -- all the arguments match exactly (perfect match)
  // 1 -- one or more arguments match via the derived-to-base conversion
  // 2 -- one or more arguments match via the reversal to untyped
  //
  // More than one match of the same rank is ambiguous.
  //
  // Optional arguments must be last. In case of a failure the function is
  // expected to issue diagnostics and throw failed. Note that the arguments
  // are conceptually "moved" and can be reused by the implementation.
  //
  // A function can also optionally receive the current scope by having the
  // first argument of the const scope* type. It may be NULL if the function
  // is called out of any scope (e.g., command line).
  //
  // Note also that we don't pass the location to the function instead
  // printing the info message pointing to the call site.
  //
  // A function can return value or anything that can be converted to value.
  // In particular, if a function returns optional<T>, then the result will be
  // either NULL or value of type T.
  //
  // Normally functions come in families that share a common qualification
  // (e.g., string. or path.). The function_family class is a "registrar"
  // that simplifies handling of function families. For example:
  //
  // function_family f ("string");
  //
  // // Register length() and string.length().
  // //
  // f["length"] += &string::size;
  //
  // // Register string.max_size().
  // //
  // f[".max_size"] += []() {return string ().max_size ();};
  //
  // The use of += instead of = is meant to suggest that we are adding an
  // overload. For more examples/ideas, study the existing function families
  // (reside in the functions-*.cxx files).
  //
  // Note that normally there will be a function overload that has all the
  // parameters untyped with an implementation that falls back to one of the
  // overloads that have all the parameters typed, possibly inferring the type
  // from the argument value "syntax" (e.g., presence of a trailing slash for
  // a directory path).
  //
  // A function is pure if for the same set of arguments it always produces
  // the same result and has no (observable) side effects. Those functions
  // that are not pure should be explicitly marked as such, for example:
  //
  // f.insert ("date", false /* pure */) += &date;
  //
  struct function_overload;

  using function_impl = value (const scope*,
                               vector_view<value>,
                               const function_overload&);

  struct LIBBUILD2_SYMEXPORT function_overload
  {
    const char* name;     // Set to point to key by insert() below.
    const char* alt_name; // Alternative name, NULL if none. This is the
                          // qualified name for unqualified or vice verse.

    // Arguments.
    //
    // A function can have a number of optional arguments. Arguments can also
    // be typed. A non-existent entry in arg_types means a value of any type.
    // A NULL entry means an untyped value.
    //
    // If arg_max equals to arg_variadic, then the function takes an unlimited
    // number of arguments. In this case the semantics of arg_min and
    // arg_types is unchanged.
    //
    static const size_t arg_variadic = size_t (~0);

    using types = vector_view<const optional<const value_type*>>;

    size_t arg_min;
    size_t arg_max;
    types  arg_types;

    // Function implementation.
    //
    function_impl* impl;

    // Auxiliary data storage. Note that it is expected to be trivially
    // copyable and destructible.
    //
    static const size_t data_size = sizeof (void*) * 3;
    alignas (std::max_align_t) unsigned char data[data_size];

    function_overload (const char* an,
                       size_t mi, size_t ma, types ts,
                       function_impl* im)
        : alt_name (an),
          arg_min (mi), arg_max (ma), arg_types (move (ts)),
          impl (im) {}

    template <typename D>
    function_overload (const char* an,
                       size_t mi, size_t ma, types ts,
                       function_impl* im,
                       D d)
        : function_overload (an, mi, ma, move (ts), im)
    {
      static_assert (sizeof (D) <= data_size, "insufficient space for data");

      // These tests appear to be broken in VC16 and also in GCC up to 5 for
      // pointers to members.
      //
#if !((defined(_MSC_VER) && _MSC_VER < 2000) ||                       \
      (defined(__GNUC__) && !defined(__clang__) && __GNUC__ <= 5))

      static_assert (std::is_trivially_copyable<D>::value,
                     "data is not trivially copyable");

      static_assert (std::is_trivially_destructible<D>::value,
                     "data is not trivially destructible");
#endif

      new (&data) D (move (d));
    }
  };

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const function_overload&); // Print signature.

  struct function_overloads: small_vector<function_overload, 8>
  {
    const char* name; // Set to point to key by function_map::insert() below.
    bool pure = true;

    function_overload&
    insert (function_overload f)
    {
      // Sanity checks.
      //
      assert (f.arg_min <= f.arg_max           &&
              f.arg_types.size () <= f.arg_max &&
              f.impl != nullptr);

      push_back (move (f));
      back ().name = name;
      return back ();
    }
  };

  class LIBBUILD2_SYMEXPORT function_map
  {
  public:
    using map_type = map<string, function_overloads>;
    using iterator = map_type::iterator;
    using const_iterator = map_type::const_iterator;

    function_overloads&
    insert (string name, bool pure)
    {
      auto p (map_.emplace (move (name), function_overloads ()));

      function_overloads& r (p.first->second);

      if (p.second)
      {
        r.name = p.first->first.c_str ();
        r.pure = pure;
      }
      else
        assert (r.pure == pure);

      return r;
    }

    const function_overloads*
    find (const string& name) const
    {
      auto i (map_.find (name));
      return i != map_.end () ? &i->second : nullptr;
    }

    value
    call (const scope* base,
          const string& name,
          vector_view<value> args,
          const location& l) const
    {
      return call (base, name, args, l, true).first;
    }

    // As above but do not fail if no match was found (but still do if the
    // match is ambiguous). Instead return an indication of whether the call
    // was made. Used to issue custom diagnostics when calling internal
    // functions.
    //
    pair<value, bool>
    try_call (const scope* base,
              const string& name,
              vector_view<value> args,
              const location& l) const
    {
      return call (base, name, args, l, false);
    }

    iterator
    begin () {return map_.begin ();}

    iterator
    end () {return map_.end ();}

    const_iterator
    begin () const {return map_.begin ();}

    const_iterator
    end () const {return map_.end ();}

    // Return true if the function with this name is already defined. If the
    // name ends with '.', then instead check if any function with this prefix
    // (which we call a family) is already defined.
    //
    bool
    defined (const string&) const;

  private:
    pair<value, bool>
    call (const scope*,
          const string&,
          vector_view<value>,
          const location&,
          bool fail) const;

    map_type map_;
  };

  LIBBUILD2_SYMEXPORT void
  insert_builtin_functions (function_map&);

  class LIBBUILD2_SYMEXPORT function_family
  {
  public:
    // The call() function above catches invalid_argument and issues
    // diagnostics by assuming it is related to function arguments and
    // contains useful description.
    //
    // In order to catch additional exceptions, you can implement a custom
    // thunk which would normally call this default implementation.
    //
    static value
    default_thunk (const scope*, vector_view<value>, const function_overload&);

    // A function family uses a common qualification (though you can pass
    // empty string to supress it). For an unqualified name (doesn't contain
    // dot) the qualified version is added automatically. A name containing a
    // leading dot is a shortcut notation for a qualified-only name.
    //
    function_family (function_map& map,
                     string qual,
                     function_impl* thunk = &default_thunk)
      : map_ (map), qual_ (move (qual)), thunk_ (thunk) {}

    struct entry;

    entry
    operator[] (string name) const;

    entry
    insert (string name, bool pure = true) const;

    static bool
    defined (function_map& map, string qual)
    {
      qual += '.';
      return map.defined (qual);
    }

  private:
    function_map& map_;
    const string qual_;
    function_impl* thunk_;
  };

  // Implementation details. If you can understand and explain all of this,
  // then you are hired ;-)!
  //

  template <typename T>
  struct function_arg
  {
    static const bool null = false;
    static const bool opt = false;

    static constexpr optional<const value_type*>
    type () {return &value_traits<T>::value_type;}

    static T&&
    cast (value* v)
    {
      if (v->null)
        throw invalid_argument ("null value");

      // Use fast but unchecked cast since the caller matched the types.
      //
      return move (v->as<T> ());
    }
  };

  template <>
  struct LIBBUILD2_SYMEXPORT function_arg<names> // Untyped.
  {
    static const bool null = false;
    static const bool opt = false;

    static constexpr optional<const value_type*>
    type () {return nullptr;}

    static names&&
    cast (value* v)
    {
      if (v->null)
        throw invalid_argument ("null value");

      return move (v->as<names> ());
    }
  };

  template <>
  struct LIBBUILD2_SYMEXPORT function_arg<value> // Anytyped.
  {
    static const bool null = false;
    static const bool opt = false;

    static constexpr optional<const value_type*>
    type () {return nullopt;}

    static value&&
    cast (value* v)
    {
      if (v->null)
        throw invalid_argument ("null value");

      return move (*v);
    }
  };

  template <typename T>
  struct function_arg<T*>: function_arg<T>
  {
    static const bool null = true;

    static T*
    cast (value* v)
    {
      if (v->null)
        return nullptr;

      // This looks bizarre but makes sense. The cast() that we are calling
      // returns an r-value reference to (what's inside) v. And it has to
      // return an r-value reference to that the value is moved into by-value
      // arguments.
      //
      T&& r (function_arg<T>::cast (v));
      return &r;
    }
  };

  template <>
  struct LIBBUILD2_SYMEXPORT function_arg<value*>: function_arg<value>
  {
    static const bool null = true;

    static value*
    cast (value* v) {return v;} // NULL indicator in value::null.
  };

  template <typename T>
  struct function_arg<optional<T>>: function_arg<T>
  {
    static const bool opt = true;

    static optional<T>
    cast (value* v)
    {
      return v != nullptr ? optional<T> (function_arg<T>::cast (v)) : nullopt;
    }
  };

  // Number of optional arguments. Note that we currently don't check that
  // they are all at the end.
  //
  template <typename A0, typename... A>
  struct function_args_opt
  {
    static const size_t count = (function_arg<A0>::opt ? 1 : 0) +
      function_args_opt<A...>::count;
  };

  template <typename A0>
  struct function_args_opt<A0>
  {
    static const size_t count = (function_arg<A0>::opt ? 1 : 0);
  };

  // Argument counts/types.
  //
  template <typename... A>
  struct function_args
  {
    static const size_t max = sizeof...(A);
    static const size_t min = max - function_args_opt<A...>::count;

    // VC15 doesn't realize that a pointer to static object (in our case it is
    // &value_trair<T>::value_type) is constexpr.
    //
    // Note that during the library split we discovered that the constexpr
    // variant causes compilation/linkage issues for both MinGW GCC and
    // VC. Thus we now only use it for POSIX systems.
    //
    // #if !defined(_MSC_VER) || _MSC_VER > 1910
    //
#if !defined(_WIN32)
    static constexpr const optional<const value_type*> types[max] = {
      function_arg<A>::type ()...};
#else
    static const optional<const value_type*> types[max];
#endif
  };

  template <typename... A>
#if !defined(_WIN32)
  constexpr const optional<const value_type*>
  function_args<A...>::types[function_args<A...>::max];
#else
  const optional<const value_type*>
  function_args<A...>::types[function_args<A...>::max] = {
    function_arg<A>::type ()...};
#endif

  // Specialization for no arguments.
  //
  template <>
  struct LIBBUILD2_SYMEXPORT function_args<>
  {
    static const size_t max = 0;
    static const size_t min = 0;

#if !defined(_WIN32)
    static constexpr const optional<const value_type*>* types = nullptr;
#else
    static const optional<const value_type*>* const types;
#endif
  };

  // Cast data/thunk for functions.
  //
  template <typename R, typename... A>
  struct function_cast_func
  {
    // A pointer to a standard layout struct is a pointer to its first data
    // member, which in our case is the cast thunk.
    //
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      R (*const impl) (A...);
    };

    static value
    thunk (const scope*, vector_view<value> args, const void* d)
    {
      return thunk (move (args),
                    static_cast<const data*> (d)->impl,
                    std::index_sequence_for<A...> ());
    }

    template <size_t... i>
    static value
    thunk (vector_view<value> args,
           R (*impl) (A...),
           std::index_sequence<i...>)
    {
      return value (
        impl (
          function_arg<A>::cast (
            i < args.size () ? &args[i] : nullptr)...));
    }
  };

  // Specialization for functions that expect the current scope as a first
  // argument.
  //
  template <typename R, typename... A>
  struct function_cast_func<R, const scope*, A...>
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      R (*const impl) (const scope*, A...);
    };

    static value
    thunk (const scope* base, vector_view<value> args, const void* d)
    {
      return thunk (base, move (args),
                    static_cast<const data*> (d)->impl,
                    std::index_sequence_for<A...> ());
    }

    template <size_t... i>
    static value
    thunk (const scope* base, vector_view<value> args,
           R (*impl) (const scope*, A...),
           std::index_sequence<i...>)
    {
      return value (
        impl (base,
              function_arg<A>::cast (
                i < args.size () ? &args[i] : nullptr)...));
    }
  };

  // Specialization for void return type. In this case we return NULL value.
  //
  template <typename... A>
  struct function_cast_func<void, A...>
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      void (*const impl) (A...);
    };

    static value
    thunk (const scope*, vector_view<value> args, const void* d)
    {
      thunk (move (args),
             static_cast<const data*> (d)->impl,
             std::index_sequence_for<A...> ());
      return value (nullptr);
    }

    template <size_t... i>
    static void
    thunk (vector_view<value> args,
           void (*impl) (A...),
           std::index_sequence<i...>)
    {
      impl (function_arg<A>::cast (i < args.size () ? &args[i] : nullptr)...);
    }
  };

  template <typename... A>
  struct function_cast_func<void, const scope*, A...>
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      void (*const impl) (const scope*, A...);
    };

    static value
    thunk (const scope* base, vector_view<value> args, const void* d)
    {
      thunk (base, move (args),
             static_cast<const data*> (d)->impl,
             std::index_sequence_for<A...> ());
      return value (nullptr);
    }

    template <size_t... i>
    static void
    thunk (const scope* base, vector_view<value> args,
           void (*impl) (const scope*, A...),
           std::index_sequence<i...>)
    {
      impl (base,
            function_arg<A>::cast (i < args.size () ? &args[i] : nullptr)...);
    }
  };

  // Customization for coerced lambdas (see below).
  //
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 6
  template <typename L, typename R, typename... A>
  struct function_cast_lamb
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      R (L::*const impl) (A...) const;
    };

    static value
    thunk (const scope*, vector_view<value> args, const void* d)
    {
      return thunk (move (args),
                    static_cast<const data*> (d)->impl,
                    std::index_sequence_for<A...> ());
    }

    template <size_t... i>
    static value
    thunk (vector_view<value> args,
           R (L::*impl) (A...) const,
           std::index_sequence<i...>)
    {
      const L* l (nullptr); // Undefined behavior.

      return value (
        (l->*impl) (
          function_arg<A>::cast (
            i < args.size () ? &args[i] : nullptr)...));
    }
  };

  template <typename L, typename R, typename... A>
  struct function_cast_lamb<L, R, const scope*, A...>
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      R (L::*const impl) (const scope*, A...) const;
    };

    static value
    thunk (const scope* base, vector_view<value> args, const void* d)
    {
      return thunk (base, move (args),
                    static_cast<const data*> (d)->impl,
                    std::index_sequence_for<A...> ());
    }

    template <size_t... i>
    static value
    thunk (const scope* base, vector_view<value> args,
           R (L::*impl) (const scope*, A...) const,
           std::index_sequence<i...>)
    {
      const L* l (nullptr); // Undefined behavior.

      return value (
        (l->*impl) (base,
                    function_arg<A>::cast (
                      i < args.size () ? &args[i] : nullptr)...));
    }
  };

  template <typename L, typename... A>
  struct function_cast_lamb<L, void, A...>
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      void (L::*const impl) (A...) const;
    };

    static value
    thunk (const scope*, vector_view<value> args, const void* d)
    {
      thunk (move (args),
             static_cast<const data*> (d)->impl,
             std::index_sequence_for<A...> ());
      return value (nullptr);
    }

    template <size_t... i>
    static void
    thunk (vector_view<value> args,
           void (L::*impl) (A...) const,
           std::index_sequence<i...>)
    {
      const L* l (nullptr);
      (l->*impl) (
        function_arg<A>::cast (
          i < args.size () ? &args[i] : nullptr)...);
    }
  };

  template <typename L, typename... A>
  struct function_cast_lamb<L, void, const scope*, A...>
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      void (L::*const impl) (const scope*, A...) const;
    };

    static value
    thunk (const scope* base, vector_view<value> args, const void* d)
    {
      thunk (base, move (args),
             static_cast<const data*> (d)->impl,
             std::index_sequence_for<A...> ());
      return value (nullptr);
    }

    template <size_t... i>
    static void
    thunk (const scope* base, vector_view<value> args,
           void (L::*impl) (const scope*, A...) const,
           std::index_sequence<i...>)
    {
      const L* l (nullptr);
      (l->*impl) (base,
                  function_arg<A>::cast (
                    i < args.size () ? &args[i] : nullptr)...);
    }
  };
#endif

  // Customization for member functions.
  //
  template <typename R, typename T>
  struct function_cast_memf
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      R (T::*const impl) () const;
    };

    static value
    thunk (const scope*, vector_view<value> args, const void* d)
    {
      auto mf (static_cast<const data*> (d)->impl);
      return value ((function_arg<T>::cast (&args[0]).*mf) ());
    }
  };

  template <typename T>
  struct function_cast_memf<void, T>
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      void (T::*const impl) () const;
    };

    static value
    thunk (const scope*, vector_view<value> args, const void* d)
    {
      auto mf (static_cast<const data*> (d)->impl);
      (function_arg<T>::cast (args[0]).*mf) ();
      return value (nullptr);
    }
  };

  // Customization for data members.
  //
  template <typename R, typename T>
  struct function_cast_memd
  {
    struct data
    {
      value (*const thunk) (const scope*, vector_view<value>, const void*);
      R T::*const impl;
    };

    static value
    thunk (const scope*, vector_view<value> args, const void* d)
    {
      auto dm (static_cast<const data*> (d)->impl);
      return value (move (function_arg<T>::cast (&args[0]).*dm));
    }
  };

  struct LIBBUILD2_SYMEXPORT function_family::entry
  {
    function_overloads& overloads;
    function_overloads* alt_overloads;
    function_impl*      thunk;

    template <typename R, typename... A>
    void
    operator+= (R (*impl) (A...)) const
    {
      using args = function_args<A...>;
      using cast = function_cast_func<R, A...>;

      insert (function_overload (
                nullptr,
                args::min,
                args::max,
                function_overload::types (args::types, args::max),
                thunk,
                typename cast::data {&cast::thunk, impl}));
    }

    template <typename R, typename... A>
    void
    operator+= (R (*impl) (const scope*, A...)) const
    {
      using args = function_args<A...>;
      using cast = function_cast_func<R, const scope*, A...>;

      insert (function_overload (
                nullptr,
                args::min,
                args::max,
                function_overload::types (args::types, args::max),
                thunk,
                typename cast::data {&cast::thunk, impl}));
    }

    // Support for assigning a (capture-less) lambda.
    //
    // GCC up until version 6 has a bug (#62052) that is triggered by calling
    // a lambda that takes a by-value argument via its "decayed" function
    // pointer. To work around this we are not going to decay it and instead
    // will call its operator() on NULL pointer; yes, undefined behavior, but
    // better than a guaranteed crash.
    //
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 6
    template <typename L>
    void
    operator+= (const L&) const
    {
      this->coerce_lambda (&L::operator());
    }

    template <typename L, typename R, typename... A>
    void
    coerce_lambda (R (L::*op) (A...) const) const
    {
      using args = function_args<A...>;
      using cast = function_cast_lamb<L, R, A...>;

      insert (function_overload (
                nullptr,
                args::min,
                args::max,
                function_overload::types (args::types, args::max),
                thunk,
                typename cast::data {&cast::thunk, op}));
    }

    template <typename L, typename R, typename... A>
    void
    coerce_lambda (R (L::*op) (const scope*, A...) const) const
    {
      using args = function_args<A...>;
      using cast = function_cast_lamb<L, R, const scope*, A...>;

      insert (function_overload (
                nullptr,
                args::min,
                args::max,
                function_overload::types (args::types, args::max),
                thunk,
                typename cast::data {&cast::thunk, op}));
    }
#else
    template <typename L>
    void
    operator+= (const L& l) const
    {
      this->operator+= (decay_lambda (&L::operator(), l));
    }

    template <typename L, typename R, typename... A>
    static auto
    decay_lambda (R (L::*) (A...) const, const L& l) -> R (*) (A...)
    {
      return static_cast<R (*) (A...)> (l);
    }
#endif

    // Support for assigning a pointer to member function (e.g. an accessor).
    //
    // For now we don't support passing additional (to this) arguments though
    // we could probably do that. The issues would be the argument passing
    // semantics (e.g., what if it's const&) and the optional/default argument
    // handling.
    //
    template <typename R, typename T>
    void
    operator+= (R (T::*mf) () const) const
    {
      using args = function_args<T>;
      using cast = function_cast_memf<R, T>;

      insert (function_overload (
                nullptr,
                args::min,
                args::max,
                function_overload::types (args::types, args::max),
                thunk,
                typename cast::data {&cast::thunk, mf}));
    }

    // Support for assigning a pointer to data member.
    //
    template <typename R, typename T>
    void
    operator+= (R T::*dm) const
    {
      using args = function_args<T>;
      using cast = function_cast_memd<R, T>;

      insert (function_overload (
                nullptr,
                args::min,
                args::max,
                function_overload::types (args::types, args::max),
                thunk,
                typename cast::data {&cast::thunk, dm}));
    }

    // Low-level interface that can be used to pass additional data.
    //
    // Note that the call to this function sidesteps the thunk. One notable
    // consequence of this is that the values are not checked for NULL.
    //
    template <typename D, typename... A>
    void
    insert (function_impl* i, D d) const
    {
      using args = function_args<A...>;

      insert (function_overload (
                nullptr,
                args::min,
                args::max,
                function_overload::types (args::types, args::max),
                i,
                move (d)));
    }

  private:
    void
    insert (function_overload f) const
    {
      function_overload* f1 (alt_overloads != nullptr
                             ? &alt_overloads->insert (f)
                             : nullptr);
      function_overload& f2 (overloads.insert (move (f)));

      // If we have both, then set alternative names.
      //
      if (f1 != nullptr)
      {
        f1->alt_name = f2.name;
        f2.alt_name = f1->name;
      }
    }
  };

  inline auto function_family::
  operator[] (string name) const -> entry
  {
    return insert (move (name));
  }
}

#endif // LIBBUILD2_FUNCTION_HXX
