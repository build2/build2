// file      : libbuild2/script/regex.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCRIPT_REGEX_HXX
#define LIBBUILD2_SCRIPT_REGEX_HXX

#include <list>
#include <regex>
#include <locale>
#include <string>        // basic_string
#include <type_traits>   // make_unsigned, enable_if, is_*

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

namespace build2
{
  namespace script
  {
    namespace regex
    {
      using char_string = std::basic_string<char>;

      enum class char_flags: uint16_t
      {
        icase = 0x1, // Case-insensitive match.
        idot  = 0x2, // Invert '.' escaping.

        none = 0
      };

      // Restricts valid standard flags to just {icase}, extends with custom
      // flags {idot}.
      //
      class char_regex: public std::basic_regex<char>
      {
      public:
        using base_type = std::basic_regex<char>;

        char_regex (const char_string&, char_flags = char_flags::none);
      };

      // Newlines are line separators and are not part of the line:
      //
      // line<newline>line<newline>
      //
      // Specifically, this means that a customary trailing newline creates a
      // trailing blank line.
      //
      // All characters can inter-compare (though there cannot be regex
      // characters in the output, only in line_regex).
      //
      // Note that we assume that line_regex and the input to regex_match()
      // use the same pool.
      //
      struct line_pool
      {
        // Note that we assume the pool can be moved without invalidating
        // pointers to any already pooled entities.
        //
        // Note that we used to use unordered_set for strings but (1) there is
        // no general expectation that we will have many identical strings and
        // (2) the number of strings is not expected to be large. So that felt
        // like an overkill and we now use a list with linear search.
        //
        std::list<char_string> strings;
        std::list<char_regex> regexes;
      };

      enum class line_type
      {
        special,
        literal,
        regex
      };

      struct line_char
      {
        // Steal last two bits from the pointer to store the type.
        //
      private:
        std::uintptr_t data_;

      public:
        line_type
        type () const {return static_cast<line_type> (data_ & 0x3);}

        int
        special () const
        {
          // Stored as (shifted) int16_t. Perform steps reversed to those
          // that are described in the comment for the corresponding ctor.
          // Note that the intermediate cast to uint16_t is required to
          // portably preserve the -1 special character.
          //
          return static_cast<int16_t> (static_cast<uint16_t> (data_ >> 2));
        }

        const char_string*
        literal () const
        {
          // Note that 2 rightmost bits are used for packaging line_char
          // type. Read the comment for the corresponding ctor for details.
          //
          return reinterpret_cast<const char_string*> (
            data_ & ~std::uintptr_t (0x3));
        }

        const char_regex*
        regex () const
        {
          // Note that 2 rightmost bits are used for packaging line_char
          // type. Read the comment for the corresponding ctor for details.
          //
          return reinterpret_cast<const char_regex*> (
            data_ & ~std::uintptr_t (0x3));
        }

        static const line_char nul;
        static const line_char eof;

        // Note: creates an uninitialized value.
        //
        line_char () = default;

        // Create a special character. The argument value must be one of the
        // following ones:
        //
        // 0 (nul character)
        // -1 (EOF)
        // [()|.*+?{}\0123456789,=!] (excluding [])
        //
        // Note that the constructor is implicit to allow basic_regex to
        // implicitly construct line_chars from special char literals (in
        // particular libstdc++ appends them to an internal line_string).
        //
        // Also note that we extend the valid characters set (see above) with
        // 'p', 'n' (used by libstdc++ for positive/negative look-ahead
        // tokens representation), and '\n', '\r', u'\u2028', u'\u2029' (used
        // by libstdc++ for newline/newparagraph matching).
        //
        line_char (int);

        // Create a literal character.
        //
        // Don't copy string if already pooled.
        //
        explicit
        line_char (const char_string&, line_pool&);

        explicit
        line_char (char_string&&, line_pool&);

        explicit
        line_char (const char_string* s) // Assume already pooled.
            //
            // Steal two bits from the pointer to package line_char type.
            // Assume (and statically assert) that char_string address is a
            // multiple of four.
            //
            : data_ (reinterpret_cast <std::uintptr_t> (s) |
                     static_cast <std::uintptr_t> (line_type::literal)) {}

        // Create a regex character.
        //
        explicit
        line_char (char_regex, line_pool&);

        explicit
        line_char (const char_regex* r) // Assume already pooled.
            //
            // Steal two bits from the pointer to package line_char type.
            // Assume (and statically assert) that char_regex address is a
            // multiple of four.
            //
            : data_ (reinterpret_cast <std::uintptr_t> (r) |
                     static_cast <std::uintptr_t> (line_type::regex)) {}

        // Provide basic_regex with the ability to use line_char in a context
        // where a char value is expected (e.g., as a function argument).
        //
        // libstdc++ seems to cast special line_chars only (and such a
        // conversion is meanigfull).
        //
        // msvcrt casts line_chars of arbitrary types instead. The only
        // reasonable strategy is to return a value that differs from any
        // other that can be encountered in a regex expression and so will
        // unlikelly be misinterpreted.
        //
        operator char () const
        {
          return type () == line_type::special ? special () : '\a'; // BELL.
        }

        // Return true if the character is a syntax (special) one.
        //
        static bool
        syntax (char);

        // Provide basic_regex (such as from msvcrt) with the ability to
        // explicitly cast line_chars to implementation-specific numeric
        // types (enums, msvcrt's _Uelem, etc).
        //
        template <typename T>
        explicit
        operator T () const
        {
          assert (type () == line_type::special);
          return static_cast<T> (special ());
        }
      };

      // Perform "deep" characters comparison (for example match literal
      // character with a regex character), rather than just compare them
      // literally. At least one argument must be of a type other than regex
      // as there is no operator==() defined to compare regexes. Characters
      // of the literal type must share the same pool (strings are compared
      // by pointers not by values).
      //
      bool
      operator== (const line_char&, const line_char&);

      // Return false if arguments are equal (operator==() returns true).
      // Otherwise if types are different return the value implying that
      // special < literal < regex. If types are special or literal return
      // the result of the respective characters or strings comparison. At
      // least one argument must be of a type other than regex as there is no
      // operator<() defined to compare regexes.
      //
      // While not very natural operation for the class we have, we have to
      // provide some meaningfull semantics for such a comparison as it is
      // required by the char_traits<line_char> specialization. While we
      // could provide it right in that specialization, let's keep it here
      // for basic_regex implementations that potentially can compare
      // line_chars as they compare them with expressions of other types (see
      // below).
      //
      bool
      operator< (const line_char&, const line_char&);

      inline bool
      operator!= (const line_char& l, const line_char& r)
      {
        return !(l == r);
      }

      inline bool
      operator<= (const line_char& l, const line_char& r)
      {
        return l < r || l == r;
      }

      // Provide basic_regex (such as from msvcrt) with the ability to
      // compare line_char to a value of an integral or
      // implementation-specific enum type. In the absense of the following
      // template operators, such a comparisons would be ambigious for
      // integral types (given that there are implicit conversions
      // int->line_char and line_char->char) and impossible for enums.
      //
      // Note that these == and < operators can succeed only for a line_char
      // of the special type. For other types they always return false. That
      // in particular leads to the following case:
      //
      // (lc != c) != (lc < c || c < lc).
      //
      // Note that we can not assert line_char is of the special type as
      // basic_regex (such as from libc++) may need the ability to check if
      // arbitrary line_char belongs to some special characters range (like
      // ['0', '9']).
      //
      template <typename T>
      struct line_char_cmp
        : public std::enable_if<std::is_integral<T>::value ||
                                (std::is_enum<T>::value &&
                                 !std::is_same<T, char_flags>::value)> {};

      template <typename T, typename = typename line_char_cmp<T>::type>
      bool
      operator== (const line_char& l, const T& r)
      {
        return l.type () == line_type::special &&
          static_cast<T> (l.special ()) == r;
      }

      template <typename T, typename = typename line_char_cmp<T>::type>
      bool
      operator== (const T& l, const line_char& r)
      {
        return r.type () == line_type::special &&
          static_cast<T> (r.special ()) == l;
      }

      template <typename T, typename = typename line_char_cmp<T>::type>
      bool
      operator!= (const line_char& l, const T& r)
      {
        return !(l == r);
      }

      template <typename T, typename = typename line_char_cmp<T>::type>
      bool
      operator!= (const T& l, const line_char& r)
      {
        return !(l == r);
      }

      template <typename T, typename = typename line_char_cmp<T>::type>
      bool
      operator< (const line_char& l, const T& r)
      {
        return l.type () == line_type::special &&
          static_cast<T> (l.special ()) < r;
      }

      template <typename T, typename = typename line_char_cmp<T>::type>
      bool
      operator< (const T& l, const line_char& r)
      {
        return r.type () == line_type::special &&
          l < static_cast<T> (r.special ());
      }

      template <typename T, typename = typename line_char_cmp<T>::type>
      inline bool
      operator<= (const line_char& l, const T& r)
      {
        return l < r || l == r;
      }

      template <typename T, typename = typename line_char_cmp<T>::type>
      inline bool
      operator<= (const T& l, const line_char& r)
      {
        return l < r || l == r;
      }

      using line_string = std::basic_string<line_char>;

      // Locale that has ctype<line_char> facet installed. Used in the
      // regex_traits<line_char> specialization (see below).
      //
      class line_char_locale: public std::locale
      {
      public:
        // Create a copy of the global C++ locale.
        //
        line_char_locale ();
      };

      // Initialize the script regex global state. Should be called once
      // prior to creating objects of types from this namespace. Note: not
      // thread-safe.
      //
      void
      init ();
    }
  }
}

// Standard template specializations for line_char that are required for the
// basic_regex<line_char> instantiation.
//
namespace std
{
  template <>
  class char_traits<build2::script::regex::line_char>
  {
  public:
    using char_type  = build2::script::regex::line_char;
    using int_type   = char_type;
    using off_type   = char_traits<char>::off_type;
    using pos_type   = char_traits<char>::pos_type;
    using state_type = char_traits<char>::state_type;

    static void
    assign (char_type& c1, const char_type& c2) {c1 = c2;}

    static char_type*
    assign (char_type*, size_t, char_type);

    // Note that eq() and lt() are not constexpr (as required by C++11)
    // because == and < operators for char_type are not constexpr.
    //
    static bool
    eq (const char_type& l, const char_type& r) {return l == r;}

    static bool
    lt (const char_type& l, const char_type& r) {return l < r;}

    static char_type*
    move (char_type*, const char_type*, size_t);

    static char_type*
    copy (char_type*, const char_type*, size_t);

    static int
    compare (const char_type*, const char_type*, size_t);

    static size_t
    length (const char_type*);

    static const char_type*
    find (const char_type*, size_t, const char_type&);

    static constexpr char_type
    to_char_type (const int_type& c) {return c;}

    static constexpr int_type
    to_int_type (const char_type& c) {return int_type (c);}

    // Note that the following functions are not constexpr (as required by
    // C++11) because their return expressions are not constexpr.
    //
    static bool
    eq_int_type (const int_type& l, const int_type& r) {return l == r;}

    static int_type eof () {return char_type::eof;}

    static int_type
    not_eof (const int_type& c)
    {
      return c != char_type::eof ? c : char_type::nul;
    }
  };

  // ctype<> must be derived from both ctype_base and locale::facet (the later
  // supports ref-counting used by the std::locale implementation internally).
  //
  // msvcrt for some reason also derives ctype_base from locale::facet which
  // produces "already a base-class" warning and effectivelly breaks the
  // reference counting. So we derive from ctype_base only in this case.
  //
  template <>
  class ctype<build2::script::regex::line_char>: public ctype_base
#if !defined(_MSC_VER) || _MSC_VER >= 2000
                                              , public locale::facet
#endif
  {
    // Used by the implementation only.
    //
    using line_type = build2::script::regex::line_type;

  public:
    using char_type = build2::script::regex::line_char;

    static locale::id id;

#if !defined(_MSC_VER) || _MSC_VER >= 2000
    explicit
    ctype (size_t refs = 0): locale::facet (refs) {}
#else
    explicit
    ctype (size_t refs = 0): ctype_base (refs) {}
#endif

    // While unnecessary, let's keep for completeness.
    //
    virtual
    ~ctype () override = default;

    // The C++ standard requires the following functions to call their virtual
    // (protected) do_*() counterparts that provide the real implementations.
    // The only purpose for this indirection is to provide a user with the
    // ability to customize existing (standard) ctype facets. As we do not
    // provide such an ability, for simplicity we will omit the do_*()
    // functions and provide the implementations directly. This should be safe
    // as nobody except us could call those protected functions.
    //
    bool
    is (mask m, char_type c) const
    {
      return m ==
             (c.type () == line_type::special && c.special () >= 0 &&
              build2::digit (static_cast<char> (c.special ()))
              ? digit
              : 0);
    }

    const char_type*
    is (const char_type*, const char_type*, mask*) const;

    const char_type*
    scan_is (mask, const char_type*, const char_type*) const;

    const char_type*
    scan_not (mask, const char_type*, const char_type*) const;

    char_type
    toupper (char_type c) const {return c;}

    const char_type*
    toupper (char_type*, const char_type* e) const {return e;}

    char_type
    tolower (char_type c) const {return c;}

    const char_type*
    tolower (char_type*, const char_type* e) const {return e;}

    char_type
    widen (char c) const {return char_type (c);}

    const char*
    widen (const char*, const char*, char_type*) const;

    char
    narrow (char_type c, char def) const
    {
      return c.type () == line_type::special ? c.special () : def;
    }

    const char_type*
    narrow (const char_type*, const char_type*, char, char*) const;
  };

  // Note: the current application locale must be POSIX. Otherwise the
  // behavior is undefined.
  //
  template <>
  class regex_traits<build2::script::regex::line_char>
  {
  public:
    using char_type       = build2::script::regex::line_char;
    using string_type     = build2::script::regex::line_string;
    using locale_type     = build2::script::regex::line_char_locale;
    using char_class_type = regex_traits<char>::char_class_type;

    // Workaround for msvcrt bugs. For some reason it assumes such a members
    // to be present in a regex_traits specialization.
    //
#if defined(_MSC_VER) && _MSC_VER < 2000
    static const ctype_base::mask _Ch_upper = ctype_base::upper;
    static const ctype_base::mask _Ch_alpha = ctype_base::alpha;

    // Unsigned numeric type. msvcrt normally casts characters to this type
    // for comparing with some numeric values or for calculating an index in
    // some bit array. Luckily that all relates to the character class
    // handling that we don't support.
    //
    using _Uelem = unsigned int;
#endif

    regex_traits () = default; // Unnecessary but let's keep for completeness.

    static size_t
    length (const char_type* p) {return string_type::traits_type::length (p);}

    char_type
    translate (char_type c) const {return c;}

    // Case-insensitive matching is not supported by line_regex. So there is no
    // reason for the function to be called.
    //
    char_type
    translate_nocase (char_type c) const {assert (false); return c;}

    // Return a sort-key - the exact copy of [b, e).
    //
    template <typename I>
    string_type
    transform (I b, I e) const {return string_type (b, e);}

    // Return a case-insensitive sort-key. Case-insensitive matching is not
    // supported by line_regex. So there is no reason for the function to be
    // called.
    //
    template <typename I>
    string_type
    transform_primary (I b, I e) const
    {
      assert (false);
      return string_type (b, e);
    }

    // POSIX regex grammar and collating elements (e.g., [.tilde.]) in
    // particular are not supported. So there is no reason for the function to
    // be called.
    //
    template <typename I>
    string_type
    lookup_collatename (I, I) const {assert (false); return string_type ();}

    // Character classes (e.g., [:lower:]) are not supported. So there is no
    // reason for the function to be called.
    //
    template <typename I>
    char_class_type
    lookup_classname (I, I, bool = false) const
    {
      assert (false);
      return char_class_type ();
    }

    // Return false as we don't support character classes (e.g., [:lower:]).
    //
    bool
    isctype (char_type, char_class_type) const {return false;}

    int
    value (char_type, int) const;

    // Return the locale passed as an argument as we do not expect anything
    // other than POSIX locale, that we also assume to be imbued by default.
    //
    locale_type
    imbue (locale_type l) {return l;}

    locale_type
    getloc () const {return locale_type ();}
  };

  // We assume line_char to be an unsigned type and express that with the
  // following specialization used by basic_regex implementations.
  //
  // libstdc++ defines unsigned CharT type (regex_traits template parameter)
  // to use as an index in some internal cache regardless if the cache is used
  // for this specialization (and the cache is used only if CharT is char).
  //
  template <>
  struct make_unsigned<build2::script::regex::line_char>
  {
    using type = build2::script::regex::line_char;
  };

  // When used with libc++ the linker complains that it can't find
  // __match_any_but_newline<line_char>::__exec() function. The problem is
  // that the function is only specialized for char and wchar_t
  // (LLVM bug #31409). As line_char has no notion of the newline character we
  // specialize the class template to behave as the __match_any<line_char>
  // instantiation does (that luckily has all the functions in place).
  //
//#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION <= 11000
#ifdef _LIBCPP_VERSION
  template <>
  class __match_any_but_newline<build2::script::regex::line_char>
    : public __match_any<build2::script::regex::line_char>
  {
  public:
    using base = __match_any<build2::script::regex::line_char>;
    using base::base;
  };
#endif
}

namespace build2
{
  namespace script
  {
    namespace regex
    {
      class line_regex: public std::basic_regex<line_char>
      {
      public:
        using base_type = std::basic_regex<line_char>;

        using base_type::base_type;

        line_regex () = default;

        // Move string regex together with the pool used to create it.
        //
        line_regex (line_string&& s, line_pool&& p)
            // No move-string ctor for base_type, so emulate it.
            //
            : base_type (s), pool (move (p)) {s.clear ();}

        // Move constuctible/assignable-only type.
        //
        line_regex (line_regex&&) = default;
        line_regex (const line_regex&) = delete;
        line_regex& operator= (line_regex&&) = default;
        line_regex& operator= (const line_regex&) = delete;

      public:
        line_pool pool;
      };
    }
  }
}

#include <libbuild2/script/regex.ixx>

#endif // LIBBUILD2_SCRIPT_REGEX_HXX
