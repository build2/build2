// file      : build2/variable.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_VARIABLE_HXX
#define BUILD2_VARIABLE_HXX

#include <map>
#include <set>
#include <type_traits>   // aligned_storage
#include <unordered_map>

#include <libbutl/prefix-map.mxx>
#include <libbutl/multi-index.mxx> // map_key

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/target-type.hxx>

namespace build2
{
  // Some general variable infrastructure rules:
  //
  // 1. A variable can only be entered or typified during the load phase.
  //
  // 2. Any entity (module) that caches a variable value must make sure the
  //    variable has already been typified.
  //
  // 3. Any entity (module) that assigns a target-specific variable value
  //    during a phase other than load must make sure the variable has already
  //    been typified.

  class value;
  struct variable;
  struct lookup;

  struct value_type
  {
    const char* name;  // Type name for diagnostics.
    const size_t size; // Type size in value::data_ (only used for PODs).

    // Base type, if any. We have very limited support for inheritance: a
    // value can be cast to the base type. In particular, a derived/base value
    // cannot be assigned to base/derived. If not NULL, then the cast function
    // below is expected to return the base pointer if its second argument
    // points to the base's value_type.
    //
    const value_type* base_type;

    // Element type, if this is a vector.
    //
    const value_type* element_type;

    // Destroy the value. If it is NULL, then the type is assumed to be POD
    // with a trivial destructor.
    //
    void (*const dtor) (value&);

    // Copy/move constructor and copy/move assignment for data_. If NULL, then
    // assume the stored data is POD. If move is true then the second argument
    // can be const_cast and moved from. copy_assign() is only called with
    // non-NULL first argument.
    //
    void (*const copy_ctor) (value&, const value&, bool move);
    void (*const copy_assign) (value&, const value&, bool move);

    // While assign cannot be NULL, if append or prepend is NULL, then this
    // means this type doesn't support this operation. Variable is optional
    // and is provided only for diagnostics. Return true if the resulting
    // value is not empty.
    //
    void (*const assign) (value&, names&&, const variable*);
    void (*const append) (value&, names&&, const variable*);
    void (*const prepend) (value&, names&&, const variable*);

    // Reverse the value back to a vector of names. Storage can be used by the
    // implementation if necessary. Cannot be NULL.
    //
    names_view (*const reverse) (const value&, names& storage);

    // Cast value::data_ storage to value type so that the result can be
    // static_cast to const T*. If it is NULL, then cast data_ directly. Note
    // that this function is used for both const and non-const values.
    //
    const void* (*const cast) (const value&, const value_type*);

    // If NULL, then the types are compared as PODs using memcmp().
    //
    int (*const compare) (const value&, const value&);

    // If NULL, then the value is never empty.
    //
    bool (*const empty) (const value&);
  };

  // The order of the enumerators is arranged so that their integral values
  // indicate whether one is more restrictive than the other.
  //
  enum class variable_visibility: uint8_t
  {
    // Note that the search for target type/pattern-specific terminates at
    // the project boundary.
    //
    normal,  // All outer scopes.
    project, // This project (no outer projects).
    scope,   // This scope (no outer scopes).
    target,  // Target and target type/pattern-specific.
    prereq   // Prerequisite-specific.
  };

  // VC14 reports ambiguity but seems to work if we don't provide any.
  //
#if !defined(_MSC_VER) || _MSC_VER > 1900
  inline bool
  operator> (variable_visibility l, variable_visibility r)
  {
    return static_cast<uint8_t> (l) > static_cast<uint8_t> (r);
  }

  inline bool
  operator>= (variable_visibility l, variable_visibility r)
  {
    return static_cast<uint8_t> (l) >= static_cast<uint8_t> (r);
  }

  inline bool
  operator< (variable_visibility l, variable_visibility r)
  {
    return r > l;
  }

  inline bool
  operator<= (variable_visibility l, variable_visibility r)
  {
    return r >= l;
  }
#endif

  ostream&
  operator<< (ostream&, variable_visibility);

  // variable
  //
  // The two variables are considered the same if they have the same name.
  //
  // Variables can be aliases of each other in which case they form a circular
  // linked list (alias for variable without any aliases points to the
  // variable itself).
  //
  // If the variable is overridden on the command line, then override is the
  // chain of the special override variables. Their names are derived from the
  // main variable name as <name>.{__override,__prefix,__suffix} and they are
  // not entered into the var_pool. The override variables only vary in their
  // names and visibility. Their alias pointer is always NULL.
  //
  // Note also that we don't propagate the variable type to override variables
  // and we keep override values as untyped names. They get "typed" when they
  // are applied.
  //
  // We use the "modify original, override on query" model. Because of that, a
  // modified value does not necessarily represent the actual value so care
  // must be taken to re-query after (direct) modification. And because of
  // that, variables set by the C++ code are by default non-overridable.
  //
  // Initial processing including entering of global overrides happens in
  // reset() before any other variables. Project wide overrides are entered in
  // main(). Overriding happens in scope::find_override().
  //
  // NULL type and normal visibility are the defaults and can be overridden by
  // "tighter" values.
  //
  struct variable
  {
    string name;
    const variable* alias;               // Circular linked list.
    const value_type* type;              // If NULL, then not (yet) typed.
    unique_ptr<const variable> override;
    variable_visibility visibility;

    // Return true if this variable is an alias of the specified variable.
    //
    bool
    aliases (const variable& var) const
    {
      const variable* v (alias);
      for (; v != &var && v != this; v = v->alias) ;
      return v == &var;
    }
  };

  inline bool
  operator== (const variable& x, const variable& y) {return x.name == y.name;}

  inline ostream&
  operator<< (ostream& os, const variable& v) {return os << v.name;}

  //
  //
  class value
  {
  public:
    // NULL means this value is not (yet) typed.
    //
    // Atomic access is used to implement on-first-access typification of
    // values store in variable_map. Direct access as well as other functions
    // that operate on values directly all use non-atomic access.
    //
    relaxed_atomic<const value_type*> type;

    // True if there is no value.
    //
    bool null;

    // Extra data that is associated with the value that can be used to store
    // flags, etc. It is initialized to 0 and copied (but not assigned) from
    // one value to another but is otherwise untouched (not even when the
    // value is reset to NULL).
    //
    // Note: if deciding to use for something make sure it is not overlapping
    // with an existing usage.
    //
    uint16_t extra;

    explicit operator bool () const {return !null;}
    bool operator== (nullptr_t) const {return null;}
    bool operator!= (nullptr_t) const {return !null;}

    // Check in a type-independent way if the value is empty. The value must
    // not be NULL.
    //
    bool
    empty () const;

    // Creation. A default-initialzied value is NULL and can be reset back to
    // NULL by assigning nullptr. Values can be copied and copy-assigned. Note
    // that for assignment, the values' types should be the same or LHS should
    // be untyped.
    //
    //
  public:
    ~value () {*this = nullptr;}

    explicit
    value (nullptr_t = nullptr): type (nullptr), null (true), extra (0) {}

    explicit
    value (const value_type* t): type (t), null (true), extra (0) {}

    explicit
    value (names); // Create untyped value.

    explicit
    value (optional<names>);

    template <typename T>
    explicit
    value (T); // Create value of value_traits<T>::value_type type.

    template <typename T>
    explicit
    value (optional<T>);

    // Note: preserves type.
    //
    value&
    operator= (nullptr_t) {if (!null) reset (); return *this;}

    value (value&&);
    explicit value (const value&);
    value& operator= (value&&);
    value& operator= (const value&);
    value& operator= (reference_wrapper<value>);
    value& operator= (reference_wrapper<const value>);

    // Assign/Append/Prepend.
    //
  public:
    // Assign/append a typed value. For assign, LHS should be either of the
    // same type or untyped. For append, LHS should be either of the same type
    // or untyped and NULL.
    //
    template <typename T> value& operator= (T);
    template <typename T> value& operator+= (T);

    template <typename T> value& operator+= (T* v) {
      return v != nullptr ? *this += *v : *this;}

    value& operator= (const char* v) {return *this = string (v);}
    value& operator+= (const char* v) {return *this += string (v);}

    // Assign/append/prepend raw data. Variable is optional and is only used
    // for diagnostics.
    //
    void assign  (names&&, const variable*);
    void assign  (name&&, const variable*);  // Shortcut for single name.
    void append  (names&&, const variable*);
    void prepend (names&&, const variable*);


    // Implementation details, don't use directly except in representation
    // type implementations.
    //
  public:
    // Fast, unchecked cast of data_ to T.
    //
    template <typename T> T& as () & {return reinterpret_cast<T&> (data_);}
    template <typename T> T&& as () && {return move (as<T> ());}
    template <typename T> const T& as () const& {
      return reinterpret_cast<const T&> (data_);}

  public:
    // The maximum size we can store directly is sufficient for the most
    // commonly used types (string, vector, map) on all the platforms that we
    // support (each type should static assert this in its value_traits
    // specialization below). Types that don't fit will have to be handled
    // with an extra dynamic allocation.
    //
    static constexpr size_t           size_ = sizeof (name_pair);
    std::aligned_storage<size_>::type data_;

    // Make sure we have sufficient storage for untyped values.
    //
    static_assert (sizeof (names) <= size_, "insufficient space");

  private:
    void
    reset ();
  };

  // This is what we call a "value pack"; it can be created by the eval
  // context and passed as arguments to functions. Usually we will have just
  // one value.
  //
  using values = small_vector<value, 1>;

  // The values should be of the same type (or both be untyped) except NULL
  // values can also be untyped. NULL values compare equal and a NULL value
  // is always less than a non-NULL.
  //
  bool operator== (const value&, const value&);
  bool operator!= (const value&, const value&);
  bool operator<  (const value&, const value&);
  bool operator<= (const value&, const value&);
  bool operator>  (const value&, const value&);
  bool operator>= (const value&, const value&);

  // Value cast. The first three expect the value to be not NULL. The cast
  // from lookup expects the value to also be defined.
  //
  // Note that a cast to names expects the value to be untyped while a cast
  // to vector<name> -- typed.
  //
  // Why are these non-members? The cast is easier on the eyes and is also
  // consistent with the cast operators. The other two are for symmetry.
  //
  template <typename T> T& cast (value&);
  template <typename T> T&& cast (value&&);
  template <typename T> const T& cast (const value&);
  template <typename T> const T& cast (const lookup&);

  // As above but returns NULL if the value is NULL (or not defined, in
  // case of lookup).
  //
  template <typename T> T* cast_null (value&);
  template <typename T> const T* cast_null (const value&);
  template <typename T> const T* cast_null (const lookup&);

  // As above but returns empty value if the value is NULL (or not defined, in
  // case of lookup).
  //
  template <typename T> const T& cast_empty (const value&);
  template <typename T> const T& cast_empty (const lookup&);

  // As above but returns the specified default if the value is NULL (or not
  // defined, in case of lookup). Note that the return is by value, not by
  // reference.
  //
  template <typename T> T cast_default (const value&, const T&);
  template <typename T> T cast_default (const lookup&, const T&);

  // As above but returns false/true if the value is NULL (or not defined,
  // in case of lookup). Note that the template argument is only for
  // documentation and should be bool (or semantically compatible).
  //
  template <typename T> T cast_false (const value&);
  template <typename T> T cast_false (const lookup&);

  template <typename T> T cast_true (const value&);
  template <typename T> T cast_true (const lookup&);


  // Assign value type to the value. The variable is optional and is only used
  // for diagnostics.
  //
  template <typename T>
  void typify        (value&, const variable*);
  void typify        (value&, const value_type&, const variable*);
  void typify_atomic (value&, const value_type&, const variable*);

  // Remove value type from the value reversing it to names. This is similar
  // to reverse() below except that it modifies the value itself.
  //
  void untypify (value&);

  // Reverse the value back to names. The value should not be NULL and storage
  // should be empty.
  //
  vector_view<const name>
  reverse (const value&, names& storage);

  vector_view<name>
  reverse (value&, names& storage);

  // lookup
  //
  // A variable can be undefined, NULL, or contain a (potentially empty)
  // value.
  //
  class variable_map;

  struct lookup
  {
    using value_type = build2::value;

    // If vars is not NULL, then value is variable_map::value_data.
    //
    const value_type*   value;  // NULL if undefined.
    const variable*     var;    // Storage variable.
    const variable_map* vars;   // Storage map.

    bool
    defined () const {return value != nullptr;}

    // Note: returns true if defined and not NULL.
    //
    explicit operator bool () const {return defined () && !value->null;}

    const value_type& operator*  () const {return *value;}
    const value_type* operator-> () const {return value;}

    // Return true if this value belongs to the specified scope or target.
    // Note that it can also be a target type/pattern-specific value in which
    // case it won't belong to either unless we pass true as a second argument
    // to consider it belonging to a scope (note that this test is expensive).
    //
    template <typename T>
    bool
    belongs (const T& x) const {return vars == &x.vars;}

    template <typename T>
    bool
    belongs (const T& x, bool target_type_pattern) const;

    lookup (): value (nullptr), var (nullptr), vars (nullptr) {}

    template <typename T>
    lookup (const value_type& v, const variable& r, const T& x)
        : lookup (&v, &r, &x.vars) {}

    lookup (const value_type& v, const variable& r, const variable_map& m)
        : lookup (&v, &r, &m) {}

    lookup (const value_type* v, const variable* r, const variable_map* m)
        : value (v),
          var  (v != nullptr ? r : nullptr),
          vars (v != nullptr ? m : nullptr) {}
  };

  // Two lookups are equal if they point to the same variable.
  //
  inline bool
  operator== (const lookup& x, const lookup& y)
  {
    bool r (x.value == y.value);
    assert (!r || x.vars == y.vars);
    return r;
  }

  inline bool
  operator!= (const lookup& x, const lookup& y) {return !(x == y);}


  // Representation types.
  //
  // Potential optimizations:
  //
  // - Split value::operator=/+=() into const T and T&&, also overload
  //   value_traits functions that they call.
  //
  // - Specialization for vector<names> (if used and becomes critical).
  //
  template <typename T, typename E>
  struct value_traits_specialization; // enable_if'able specialization support.

  template <typename T>
  struct value_traits: value_traits_specialization <T, void> {};
  // {
  //   static_assert (sizeof (T) <= value::size_, "insufficient space");
  //
  //   // Convert name to T. If rhs is not NULL, then it is the second half
  //   // of a pair. Only needs to be provided by simple types. Throw
  //   // invalid_argument (with a message) if the name is not a valid
  //   // representation of value (in which case the name should remain
  //   // unchanged for diagnostics).
  //   //
  //   static T convert (name&&, name* rhs);
  //
  //   // Assign/append/prepend T to value which is already of type T but can
  //   // be NULL.
  //   //
  //   static void assign (value&, T&&);
  //   static void append (value&, T&&);
  //   static void prepend (value&, T&&);
  //
  //   // Reverse a value back to name. Only needs to be provided by simple
  //   // types.
  //   //
  //   static name reverse (const T&);
  //
  //   // Compare two values. Only needs to be provided by simple types.
  //   //
  //   static int compare (const T&, const T&);
  //
  //   // Return true if the value is empty.
  //   //
  //   static bool empty (const T&);
  //
  //   // True if can be constructed from empty names as T().
  //   //
  //   static const bool empty_value = true;
  //
  //   static const T empty_instance;
  //
  //   // For simple types (those that can be used as elements of containers),
  //   // type_name must be constexpr in order to sidestep the static init
  //   // order issue (in fact, that's the only reason we have it both here
  //   // and in value_type.name -- value_type cannot be constexpr because
  //   // of pointers to function template instantiations).
  //   //
  //   static const char* const type_name;
  //   static const build2::value_type value_type;
  // };

  // Convert name to a simple value. Throw invalid_argument (with a message)
  // if the name is not a valid representation of value (in which case the
  // name remains unchanged for diagnostics). The second version is called for
  // a pair.
  //
  template <typename T> T convert (name&&);
  template <typename T> T convert (name&&, name&&);

  // As above but can also be called for container types. Note that in this
  // case (container) if invalid_argument is thrown, the names are not
  // guaranteed to be unchanged.
  //
  //template <typename T> T convert (names&&); (declaration causes ambiguity)

  // Convert value to T. If value is already of type T, then simply cast it.
  // Otherwise call convert(names) above.
  //
  template <typename T> T convert (value&&);

  // Default implementations of the dtor/copy_ctor/copy_assing callbacks for
  // types that are stored directly in value::data_ and the provide all the
  // necessary functions (copy/move ctor and assignment operator).
  //
  template <typename T>
  static void
  default_dtor (value&);

  template <typename T>
  static void
  default_copy_ctor (value&, const value&, bool);

  template <typename T>
  static void
  default_copy_assign (value&, const value&, bool);

  // Default implementations of the empty callback that calls
  // value_traits<T>::empty().
  //
  template <typename T>
  static bool
  default_empty (const value&);

  // Default implementations of the assign/append/prepend callbacks for simple
  // types. They call value_traits<T>::convert() and then pass the result to
  // value_traits<T>::assign()/append()/prepend(). As a result, it may not be
  // the most efficient way to do it.
  //
  template <typename T>
  static void
  simple_assign (value&, names&&, const variable*);

  template <typename T>
  static void
  simple_append (value&, names&&, const variable*);

  template <typename T>
  static void
  simple_prepend (value&, names&&, const variable*);

  // Default implementations of the reverse callback for simple types that
  // calls value_traits<T>::reverse() and adds the result to the vector. As a
  // result, it may not be the most efficient way to do it.
  //
  template <typename T>
  static names_view
  simple_reverse (const value&, names&);

  // Default implementations of the compare callback for simple types that
  // calls value_traits<T>::compare().
  //
  template <typename T>
  static int
  simple_compare (const value&, const value&);

  // names
  //
  template <>
  struct value_traits<names>
  {
    static const names& empty_instance;
  };

  // bool
  //
  template <>
  struct value_traits<bool>
  {
    static_assert (sizeof (bool) <= value::size_, "insufficient space");

    static bool convert (name&&, name*);
    static void assign (value&, bool);
    static void append (value&, bool); // OR.
    static name reverse (bool x) {return name (x ? "true" : "false");}
    static int compare (bool, bool);
    static bool empty (bool) {return false;}

    static const bool empty_value = false;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  template <>
  struct value_traits<uint64_t>
  {
    static_assert (sizeof (uint64_t) <= value::size_, "insufficient space");

    static uint64_t convert (name&&, name*);
    static void assign (value&, uint64_t);
    static void append (value&, uint64_t); // ADD.
    static name reverse (uint64_t x) {return name (to_string (x));}
    static int compare (uint64_t, uint64_t);
    static bool empty (bool) {return false;}

    static const bool empty_value = false;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // Treat unsigned integral types as uint64. Note that bool is handled
  // differently at an earlier stage.
  //
  template <typename T>
  struct value_traits_specialization<T,
                                     typename std::enable_if<
                                       std::is_integral<T>::value &&
                                       std::is_unsigned<T>::value>::type>:
    value_traits<uint64_t> {};

  // string
  //
  template <>
  struct value_traits<string>
  {
    static_assert (sizeof (string) <= value::size_, "insufficient space");

    static string convert (name&&, name*);
    static void assign (value&, string&&);
    static void append (value&, string&&);
    static void prepend (value&, string&&);
    static name reverse (const string& x) {return name (x);}
    static int compare (const string&, const string&);
    static bool empty (const string& x) {return x.empty ();}

    static const bool empty_value = true;
    static const string& empty_instance;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // Treat const char* as string.
  //
  template <>
  struct value_traits<const char*>: value_traits<string> {};

  // path
  //
  template <>
  struct value_traits<path>
  {
    static_assert (sizeof (path) <= value::size_, "insufficient space");

    static path convert (name&&, name*);
    static void assign (value&, path&&);
    static void append (value&, path&&);  // operator/
    static void prepend (value&, path&&); // operator/
    static name reverse (const path& x) {
      return x.to_directory ()
        ? name (path_cast<dir_path> (x))
        : name (x.string ());
    }
    static int compare (const path&, const path&);
    static bool empty (const path& x) {return x.empty ();}

    static const bool empty_value = true;
    static const path& empty_instance;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // dir_path
  //
  template <>
  struct value_traits<dir_path>
  {
    static_assert (sizeof (dir_path) <= value::size_, "insufficient space");

    static dir_path convert (name&&, name*);
    static void assign (value&, dir_path&&);
    static void append (value&, dir_path&&);  // operator/
    static void prepend (value&, dir_path&&); // operator/
    static name reverse (const dir_path& x) {return name (x);}
    static int compare (const dir_path&, const dir_path&);
    static bool empty (const dir_path& x) {return x.empty ();}

    static const bool empty_value = true;
    static const dir_path& empty_instance;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // abs_dir_path
  //
  template <>
  struct value_traits<abs_dir_path>
  {
    static_assert (sizeof (abs_dir_path) <= value::size_,
                   "insufficient space");

    static abs_dir_path convert (name&&, name*);
    static void assign (value&, abs_dir_path&&);
    static void append (value&, abs_dir_path&&);  // operator/
    static name reverse (const abs_dir_path& x) {return name (x);}
    static int compare (const abs_dir_path&, const abs_dir_path&);
    static bool empty (const abs_dir_path& x) {return x.empty ();}

    static const bool empty_value = true;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // name
  //
  template <>
  struct value_traits<name>
  {
    static_assert (sizeof (name) <= value::size_, "insufficient space");

    static name convert (name&&, name*);
    static void assign (value&, name&&);
    static name reverse (const name& x) {return x;}
    static int compare (const name& l, const name& r) {return l.compare (r);}
    static bool empty (const name& x) {return x.empty ();}

    static const bool empty_value = true;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // name_pair
  //
  // An empty first or second half of a pair is treated as unspecified (this
  // way it can be usage-specific whether a single value is first or second
  // half of a pair). If both are empty then this is an empty value (and not a
  // pair of two empties).
  //
  template <>
  struct value_traits<name_pair>
  {
    static_assert (sizeof (name_pair) <= value::size_, "insufficient space");

    static name_pair convert (name&&, name*);
    static void assign (value&, name_pair&&);
    static int compare (const name_pair&, const name_pair&);
    static bool empty (const name_pair& x) {
      return x.first.empty () && x.second.empty ();}

    static const bool empty_value = true;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // process_path
  //
  // Note that instances that we store always have non-empty recall and
  // initial is its shallow copy.
  //
  template <>
  struct value_traits<process_path>
  {
    static_assert (sizeof (process_path) <= value::size_,
                   "insufficient space");

    // This one is represented as a @-pair of names. As a result it cannot
    // be stored in a container.
    //
    static process_path convert (name&&, name*);
    static void assign (value&, process_path&&);
    static int compare (const process_path&, const process_path&);
    static bool empty (const process_path& x) {return x.empty ();}

    static const bool empty_value = true;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // target_triplet
  //
  template <>
  struct value_traits<target_triplet>
  {
    static_assert (sizeof (target_triplet) <= value::size_,
                   "insufficient space");

    static target_triplet convert (name&&, name*);
    static void assign (value&, target_triplet&&);
    static name reverse (const target_triplet& x) {return name (x.string ());}
    static int compare (const target_triplet& x, const target_triplet& y) {
      return x.compare (y);}
    static bool empty (const target_triplet& x) {return x.empty ();}

    static const bool empty_value = true;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // project_name
  //
  template <>
  struct value_traits<project_name>
  {
    static_assert (sizeof (project_name) <= value::size_,
                   "insufficient space");

    static project_name convert (name&&, name*);
    static void assign (value&, project_name&&);
    static name reverse (const project_name& x) {return name (x.string ());}
    static int compare (const project_name& x, const project_name& y) {
      return x.compare (y);}
    static bool empty (const project_name& x) {return x.empty ();}

    static const bool empty_value = true;
    static const project_name& empty_instance;
    static const char* const type_name;
    static const build2::value_type value_type;
  };

  // vector<T>
  //
  template <typename T>
  struct value_traits<vector<T>>
  {
    static_assert (sizeof (vector<T>) <= value::size_, "insufficient space");

    static vector<T> convert (names&&);
    static void assign (value&, vector<T>&&);
    static void append (value&, vector<T>&&);
    static void prepend (value&, vector<T>&&);
    static bool empty (const vector<T>& x) {return x.empty ();}

    static const vector<T> empty_instance;

    // Make sure these are static-initialized together. Failed that VC will
    // make sure it's done in the wrong order.
    //
    struct value_type_ex: build2::value_type
    {
      string type_name;
      value_type_ex (value_type&&);
    };
    static const value_type_ex value_type;
  };

  // map<K, V>
  //
  template <typename K, typename V>
  struct value_traits<std::map<K, V>>
  {
    template <typename K1, typename V1> using map = std::map<K1, V1>;

    static_assert (sizeof (map<K, V>) <= value::size_, "insufficient space");

    static void assign (value&, map<K, V>&&);
    static void append (value&, map<K, V>&&);
    static void prepend (value& v, map<K, V>&& x) {
      return append (v, move (x));}
    static bool empty (const map<K, V>& x) {return x.empty ();}

    static const map<K, V> empty_instance;

    // Make sure these are static-initialized together. Failed that VC will
    // make sure it's done in the wrong order.
    //
    struct value_type_ex: build2::value_type
    {
      string type_name;
      value_type_ex (value_type&&);
    };
    static const value_type_ex value_type;
  };

  // Project-wide (as opposed to global) variable overrides. Returned by
  // context.cxx:reset().
  //
  struct variable_override
  {
    const variable&    var; // Original variable.
    const variable&    ovr; // Override variable.
    optional<dir_path> dir; // Scope directory relative to base.
    value              val;
  };

  using variable_overrides = vector<variable_override>;

  // Variable pool.
  //
  // The global version is protected by the phase mutex.
  //
  class variable_pool
  {
  public:
    // Find existing (assert exists).
    //
    const variable&
    operator[] (const string& name) const;

    // Return NULL if there is no variable with this name.
    //
    const variable*
    find (const string& name) const;

    // Find existing or insert new (untyped, non-overridable, normal
    // visibility; but may be overridden by a pattern).
    //
    const variable&
    insert (string name)
    {
      return insert (move (name), nullptr, nullptr, nullptr);
    }

    // Insert or override (type/visibility). Note that by default the
    // variable is not overridable.
    //
    const variable&
    insert (string name, variable_visibility v)
    {
      return insert (move (name), nullptr, &v, nullptr);
    }

    const variable&
    insert (string name, bool overridable)
    {
      return insert (move (name), nullptr, nullptr, &overridable);
    }

    const variable&
    insert (string name, bool overridable, variable_visibility v)
    {
      return insert (move (name), nullptr, &v, &overridable);
    }

    template <typename T>
    const variable&
    insert (string name)
    {
      return insert (move (name), &value_traits<T>::value_type);
    }

    template <typename T>
    const variable&
    insert (string name, variable_visibility v)
    {
      return insert (move (name), &value_traits<T>::value_type, &v);
    }

    template <typename T>
    const variable&
    insert (string name, bool overridable)
    {
      return insert (
        move (name), &value_traits<T>::value_type, nullptr, &overridable);
    }

    template <typename T>
    const variable&
    insert (string name, bool overridable, variable_visibility v)
    {
      return insert (
        move (name), &value_traits<T>::value_type, &v, &overridable);
    }

    // Alias an existing variable with a new name.
    //
    // Aliasing is purely a lookup-level mechanism. That is, when variable_map
    // looks for a value, it tries all the aliases (and returns the storage
    // variable in lookup).
    //
    // The existing variable should already have final type and visibility
    // values which are copied over to the alias.
    //
    // Overridable aliased variables are most likely a bad idea: without a
    // significant effort, the overrides will only be applied along the alias
    // names (i.e., there would be no cross-alias overriding). So for now we
    // don't allow this (use the common variable mechanism instead).
    //
    const variable&
    insert_alias (const variable& var, string name);

    // Insert a variable pattern. Any variable that matches this pattern
    // will have the specified type, visibility, and overridability. If
    // match is true, then individual insertions of the matching variable
    // must match the specified type/visibility/overridability. Otherwise,
    // individual insertions can provide alternative values and the pattern
    // values are a fallback (if you specify false you better be very clear
    // about what you are trying to achieve).
    //
    // The pattern must be in the form [<prefix>.](*|**)[.<suffix>] where
    // '*' matches single component stems (i.e., 'foo' but not 'foo.bar')
    // and '**' matches single and multi-component stems. Note that only
    // multi-component variables are considered for pattern matching (so
    // just '*' won't match anything).
    //
    // The patterns are matched in the more-specific-first order where the
    // pattern is considered more specific if it has a greater sum of its
    // prefix and suffix lengths. If the prefix and suffix are equal, then the
    // '*' pattern is considered more specific than '**'. If neither is more
    // specific, then they are matched in the reverse order of insertion.
    //
    // If retro is true then a newly inserted pattern is also applied
    // retrospectively to all the existing variables that match but only
    // if no more specific pattern already exists (which is then assumed
    // to have been applied). So if you use this functionality, watch out
    // for the insertion order (you probably want more specific first).
    //
  public:
    void
    insert_pattern (const string& pattern,
                    optional<const value_type*> type,
                    optional<bool> overridable,
                    optional<variable_visibility>,
                    bool retro = false,
                    bool match = true);

    template <typename T>
    void
    insert_pattern (const string& p,
                    optional<bool> overridable,
                    optional<variable_visibility> v,
                    bool retro = false,
                    bool match = true)
    {
      insert_pattern (
        p, &value_traits<T>::value_type, overridable, v, retro, match);
    }

  public:
    void
    clear () {map_.clear ();}

    variable_pool (): variable_pool (false) {}

    // RW access.
    //
    variable_pool&
    rw () const
    {
      assert (phase == run_phase::load);
      return const_cast<variable_pool&> (*this);
    }

    variable_pool&
    rw (scope&) const {return const_cast<variable_pool&> (*this);}

  private:
    static variable_pool instance;

    variable&
    insert (string name,
            const value_type*,
            const variable_visibility* = nullptr,
            const bool* overridable = nullptr,
            bool pattern = true);

    void
    update (variable&,
            const value_type*,
            const variable_visibility* = nullptr,
            const bool* = nullptr) const;

    // Entities that can access bypassing the lock proof.
    //
    friend class parser;
    friend class scope;
    friend variable_overrides reset (const strings&);

  public:
    static const variable_pool& cinstance; // For var_pool initialization.

    // Variable map.
    //
  private:
    using key = butl::map_key<string>;
    using map = std::unordered_map<key, variable>;

    pair<map::iterator, bool>
    insert (variable&& var)
    {
      // Keeping a pointer to the key while moving things during insertion is
      // tricky. We could use a C-string instead of C++ for a key but that
      // gets hairy very quickly (there is no std::hash for C-strings). So
      // let's rely on small object-optimized std::string for now.
      //
      string n (var.name);
      auto r (map_.insert (map::value_type (&n, move (var))));

      if (r.second)
        r.first->first.p = &r.first->second.name;

      return r;
    }

    map map_;

    // Patterns.
    //
  public:
    struct pattern
    {
      string prefix;
      string suffix;
      bool   multi; // Match multi-component stems.
      bool   match; // Must match individual variable insersions.

      optional<const value_type*>   type;
      optional<variable_visibility> visibility;
      optional<bool>                overridable;

      friend bool
      operator< (const pattern& x, const pattern& y)
      {
        if (x.prefix.size () + x.suffix.size () <
            y.prefix.size () + y.suffix.size ())
          return true;

        if (x.prefix == y.prefix && x.suffix == y.suffix)
          return x.multi && !y.multi;

        return false;
      }
    };

  private:
    std::multiset<pattern> patterns_;

    // Global pool flag.
    //
  private:
    explicit
    variable_pool (bool global): global_ (global) {}

    bool global_;
  };

  extern const variable_pool& var_pool;
}

// variable_map
//
namespace butl
{
  template <>
  struct compare_prefix<std::reference_wrapper<const build2::variable>>:
    compare_prefix<std::string>
  {
    typedef compare_prefix<std::string> base;

    explicit
    compare_prefix (char d): base (d) {}

    bool
    operator() (const build2::variable& x, const build2::variable& y) const
    {
      return base::operator() (x.name, y.name);
    }

    bool
    prefix (const build2::variable& p, const build2::variable& k) const
    {
      return base::prefix (p.name, k.name);
    }
  };
}

namespace build2
{
  class variable_map
  {
  public:
    struct value_data: value
    {
      using value::value;
      using value::operator=;

      size_t version = 0; // Incremented on each modification (variable_cache).
    };

    // Note that we guarantee ascending iteration order (e.g., for predictable
    // dump output in tests).
    //
    using map_type = butl::prefix_map<reference_wrapper<const variable>,
                                      value_data,
                                      '.'>;
    using size_type = map_type::size_type;

    template <typename I>
    class iterator_adapter: public I
    {
    public:
      iterator_adapter () = default;
      iterator_adapter (const I& i, const variable_map& m): I (i), m_ (&m) {}

      // Automatically type a newly typed value on access.
      //
      typename I::reference operator* () const;
      typename I::pointer   operator-> () const;

      // Untyped access.
      //
      uint16_t extra () const {return I::operator* ().second.extra;}
      typename I::reference untyped () const {return I::operator* ();}

    private:
      const variable_map* m_;
    };

    using const_iterator = iterator_adapter<map_type::const_iterator>;

    // Lookup. Note that variable overrides will not be applied, even if
    // set in this map.
    //
    lookup
    operator[] (const variable& var) const
    {
      auto p (find (var));
      return lookup (p.first, &p.second, this);
    }

    lookup
    operator[] (const variable* var) const // For cached variables.
    {
      assert (var != nullptr);
      return operator[] (*var);
    }

    lookup
    operator[] (const string& name) const
    {
      const variable* var (var_pool.find (name));
      return var != nullptr ? operator[] (*var) : lookup ();
    }

    // If typed is false, leave the value untyped even if the variable is.
    // The second half of the pair is the storage variable.
    //
    pair<const value_data*, const variable&>
    find (const variable&, bool typed = true) const;

    pair<value_data*, const variable&>
    find_to_modify (const variable&, bool typed = true);

    // Convert a lookup pointing to a value belonging to this variable map
    // to its non-const version. Note that this is only safe on the original
    // values (see find_original()).
    //
    value&
    modify (const lookup& l)
    {
      assert (l.vars == this);
      value& r (const_cast<value&> (*l.value));
      static_cast<value_data&> (r).version++;
      return r;
    }

    // Return a value suitable for assignment. See scope for details.
    //
    value&
    assign (const variable& var) {return insert (var).first;}

    value&
    assign (const variable* var) // For cached variables.
    {
      assert (var != nullptr);
      return assign (*var);
    }

    // Note that the variable is expected to have already been registered.
    //
    value&
    assign (const string& name) {return insert (var_pool[name]).first;}

    // As above but also return an indication of whether the new value (which
    // will be NULL) was actually inserted. Similar to find(), if typed is
    // false, leave the value untyped even if the variable is.
    //
    pair<reference_wrapper<value>, bool>
    insert (const variable&, bool typed = true);

    pair<const_iterator, const_iterator>
    find_namespace (const variable& ns) const
    {
      auto r (m_.find_sub (ns));
      return make_pair (const_iterator (r.first,  *this),
                        const_iterator (r.second, *this));
    }

    const_iterator
    begin () const {return const_iterator (m_.begin (), *this);}

    const_iterator
    end () const {return const_iterator (m_.end (), *this);}

    bool
    empty () const {return m_.empty ();}

    size_type
    size () const {return m_.size ();}

  public:
    // Global should be true if this map is part of the global build state
    // (e.g., scopes, etc).
    //
    explicit
    variable_map (bool global = false): global_ (global) {}

    void
    clear () {m_.clear ();}

  private:
    friend class variable_type_map;

    void
    typify (const value_data&, const variable&) const;

  private:
    bool global_;
    map_type m_;
  };

  // Value caching. Used for overrides as well as target type/pattern-specific
  // append/prepend.
  //
  // In many places we assume that we can store a reference to the returned
  // variable value (e.g., install::lookup_install()). As a result, in these
  // cases where we calculate the value dynamically, we have to cache it
  // (note, however, that if the value becomes stale, there is no guarantee
  // the references remain valid).
  //
  // Note that since the cache can be modified on any lookup (including during
  // the execute phase), it is protected by its own mutex shard (allocated in
  // main()). This shard is also used for value typification (which is kind of
  // like caching) during concurrent execution phases.
  //
  extern size_t variable_cache_mutex_shard_size;
  extern unique_ptr<shared_mutex[]> variable_cache_mutex_shard;

  template <typename K>
  class variable_cache
  {
  public:
    // If the returned unique lock is locked, then the value has been
    // invalidated. If the variable type does not match the value type,
    // then typify the cached value.
    //
    pair<value&, ulock>
    insert (K, const lookup& stem, size_t version, const variable&);

  private:
    struct entry_type
    {
      // Note: we use value_data instead of value since the result is often
      // returned as lookup. We also maintain the version in case one cached
      // value (e.g., override) is based on another (e.g., target
      // type/pattern-specific prepend/append).
      //
      variable_map::value_data value;

      size_t version = 0; // Version on which this value is based.

      // Location of the stem as well as the version on which this cache
      // value is based. Used to track the location and value of the stem
      // for cache invalidation. NULL/0 means there is no stem.
      //
      const variable_map* stem_vars = nullptr;
      size_t              stem_version = 0;

      // For GCC 4.9.
      //
      entry_type () = default;
      entry_type (variable_map::value_data val,
                  size_t ver,
                  const variable_map* svars,
                  size_t sver)
          : value (move (val)),
            version (ver),
            stem_vars (svars),
            stem_version (sver) {}
    };

    using map_type = std::map<K, entry_type>;

    map_type m_;
  };

  // Target type/pattern-specific variables.
  //
  class variable_pattern_map
  {
  public:
    using map_type = std::map<string, variable_map>;
    using const_iterator = map_type::const_iterator;
    using const_reverse_iterator = map_type::const_reverse_iterator;

    explicit
    variable_pattern_map (bool global): global_ (global) {}

    variable_map&
    operator[] (const string& v)
    {
      return map_.emplace (v, variable_map (global_)).first->second;
    }

    const_iterator         begin ()  const {return map_.begin ();}
    const_iterator         end ()    const {return map_.end ();}
    const_reverse_iterator rbegin () const {return map_.rbegin ();}
    const_reverse_iterator rend ()   const {return map_.rend ();}
    bool                   empty ()  const {return map_.empty ();}

  private:
    bool global_;
    map_type map_;
  };

  class variable_type_map
  {
  public:
    using map_type = std::map<reference_wrapper<const target_type>,
                              variable_pattern_map>;
    using const_iterator = map_type::const_iterator;

    explicit
    variable_type_map (bool global): global_ (global) {}

    variable_pattern_map&
    operator[] (const target_type& t)
    {
      return map_.emplace (t, variable_pattern_map (global_)).first->second;
    }

    const_iterator begin () const {return map_.begin ();}
    const_iterator end ()   const {return map_.end ();}
    bool           empty () const {return map_.empty ();}

    lookup
    find (const target_type&, const string& tname, const variable&) const;

    // Prepend/append value cache.
    //
    // The key is the combination of the "original value identity" (as a
    // pointer to the value in one of the variable_pattern_map's) and the
    // "target identity" (as target type and target name). Note that while at
    // first it may seem like we don't need the target identity, we actually
    // do since the stem may itself be target-type/pattern-specific. See
    // scope::find_original() for details.
    //
    mutable
    variable_cache<tuple<const value*, const target_type*, string>>
    cache;

  private:
    bool global_;
    map_type map_;
  };
}

#include <build2/variable.ixx>
#include <build2/variable.txx>

#endif // BUILD2_VARIABLE_HXX
