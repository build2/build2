// file      : libbuild2/token.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TOKEN_HXX
#define LIBBUILD2_TOKEN_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Token type.
  //
  // A line consists of a sequence of words separated by separators and
  // terminated with the newline. If whitespace is a separator, then it is
  // ignored.
  //
  struct token_type
  {
    // Extendable/inheritable enum-like class.
    //
    enum
    {
      // NOTE: remember to update token_printer()!

      eos,
      newline,
      word,
      escape,          // token::value is <...> in $\<...>
      pair_separator,  // token::value[0] is the pair separator char.

      colon,           // :
      dollar,          // $
      question,        // ?
      percent,         // %
      comma,           // ,
      backtick,        // `

      lparen,          // (
      rparen,          // )

      lcbrace,         // {
      rcbrace,         // }

      multi_lcbrace,   // {{... (value contains the braces)
      multi_rcbrace,   // }}... (value contains the braces)

      lsbrace,         // [
      rsbrace,         // ]

      labrace,         // <
      rabrace,         // >

      assign,          // =
      prepend,         // =+
      append,          // +=
      default_assign,  // ?=

      equal,           // ==
      not_equal,       // !=
      less,            // <
      greater,         // >
      less_equal,      // <=
      greater_equal,   // >=

      bit_or,          // |

      log_or,          // ||
      log_and,         // &&
      log_not,         // !

      value_next
    };

    using value_type = uint16_t;

    token_type (value_type v = eos): v_ (v) {}
    operator value_type () const {return v_;}
    value_type v_;
  };

  // Token can be unquoted, single-quoted ('') or double-quoted (""). It can
  // also be mixed.
  //
  enum class quote_type {unquoted, single, double_, mixed};

  class token;

  enum class print_mode
  {
    // Print eos, newline, and pair separator in the <name> form and other
    // tokens as literals, single-quoting the word token.
    //
    normal,

    // Same as normal but all literals are quoted.
    //
    diagnostics,

    // Print all tokens as literals with newline represented as '\n' and eos
    // as an empty string.
    //
    raw
  };

  LIBBUILD2_SYMEXPORT void
  token_printer (ostream&, const token&, print_mode);

  class token
  {
  public:
    using printer_type = void (ostream&, const token&, print_mode);

    token_type type;
    bool separated; // Whitespace-separated from the previous token.

    // Quoting can be complete, where the token starts and ends with the quote
    // characters and quoting is contiguous or partial where only some part(s)
    // of the token are quoted or quoting continues to the next token. We also
    // keep track whether the first character of a token is quoted (we also
    // treat escaped first character as quoted).
    //
    quote_type qtype;
    bool qcomp;
    bool qfirst;

    // Normally only used for word, but can also be used to store "modifiers"
    // or some such for other tokens.
    //
    string value;

    uint64_t line;
    uint64_t column;

    printer_type* printer;

  public:
    token ()
        : token (token_type::eos, false, 0, 0, token_printer) {}

    token (token_type t, bool s, uint64_t l, uint64_t c, printer_type* p)
        : token (t, string (), s,
                 quote_type::unquoted, false, false,
                 l, c,
                 p) {}

    token (token_type t, bool s,
           quote_type qt,
           uint64_t l, uint64_t c,
           printer_type* p)
        : token (t, string (), s,
                 qt, qt != quote_type::unquoted, qt != quote_type::unquoted,
                 l, c,
                 p) {}

    token (string v, bool s,
           quote_type qt, bool qc, bool qf,
           uint64_t l, uint64_t c)
        : token (token_type::word, move (v), s, qt, qc, qf, l, c) {}

    token (token_type t,
           string v, bool s,
           quote_type qt, bool qc, bool qf,
           uint64_t l, uint64_t c,
           printer_type* p = &token_printer)
        : type (t), separated (s),
          qtype (qt), qcomp (qc), qfirst (qf),
          value (move (v)),
          line (l), column (c),
          printer (p) {}
  };

  // Output the token value in a format suitable for diagnostics.
  //
  inline ostream&
  operator<< (ostream& o, const token& t)
  {
    t.printer (o, t, print_mode::diagnostics);
    return o;
  }

  // Note: these are currently only used for sanity checks.
  //
  inline bool
  operator== (const token& x, const token& y)
  {
    return x.type == y.type && x.value == y.value;
  }

  inline bool
  operator!= (const token& x, const token& y)
  {
    return !(x == y);
  }

  // Context-dependent lexing (see lexer_mode for details).
  //
  struct lexer_mode_base
  {
    // Extendable/inheritable enum-like class.
    //
    enum { value_next };

    using value_type = uint16_t;

    lexer_mode_base (value_type v = value_next): v_ (v) {}
    operator value_type () const {return v_;}
    value_type v_;
  };

  struct replay_token
  {
    build2::token token;
    const path_name* file;
    lexer_mode_base mode;
    uintptr_t       mode_data;

    using location_type = build2::location;

    location_type
    location () const {return location_type (*file, token.line, token.column);}
  };

  using replay_tokens = vector<replay_token>;

  // Diagnostics plumbing. We assume that any diag stream for which we can use
  // token as location has its aux data pointing to pointer to path name.
  //
  inline location
  get_location (const token& t, const path_name& pn)
  {
    return location (pn, t.line, t.column);
  }

  inline location
  get_location (const token& t, const void* data)
  {
    assert (data != nullptr); // E.g., must be &parser::path_.
    const path_name* pn (*static_cast<const path_name* const*> (data));
    return get_location (t, *pn);
  }
}

#endif // LIBBUILD2_TOKEN_HXX
