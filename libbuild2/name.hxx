// file      : libbuild2/name.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

// Note: include <libbuild2/types.hxx> instead of this file directly.
//

#ifndef LIBBUILD2_NAME_HXX
#define LIBBUILD2_NAME_HXX

// We cannot include <libbuild2/utility.hxx> since it includes
// <libbuild2/types.hxx>.
//
#include <utility> // move()

#include <libbuild2/export.hxx>

namespace build2
{
  using std::move;

  // A name is what we operate on by default. Depending on the context, it can
  // be interpreted as a target or prerequisite name. A name without a type
  // and directory can be used to represent any text. A name with directory
  // and empty value represents a directory.
  //
  // A name may also be qualified with a project. If the project name is
  // empty, then it means the name is in a project other than our own (e.g.,
  // it is installed).
  //
  // A type can only be specified if either directory or value are not empty.
  // We allow project-qualified empty names for reversibility.
  //
  // If pair is not '\0', then this name and the next in the list form a
  // pair. Can be used as a bool flag.
  //
  // If pattern is present then this is a name pattern (e.g., file{*.txt},
  // file{~'/(.+)\.txt/i'}, file{^'/\1/'}). A directory name cannot be a regex
  // pattern (since we would need to store it in dir_path and a regex is not
  // necessarily a valid path).
  //
  struct name
  {
    enum class pattern_type: uint8_t {path, regex_pattern, regex_substitution};

    optional<project_name> proj;
    dir_path dir;
    string type;
    string value;
    char pair = '\0';
    optional<pattern_type> pattern;

    name () {} // = default; Clang needs this to initialize const object.
    name (string v): value (move (v)) {}
    name (dir_path d): dir (move (d)) {}
    name (string t, string v): type (move (t)), value (move (v)) {}
    name (dir_path d, string v): dir (move (d)), value (move (v)) {}

    name (dir_path d, string t, string v)
        : dir (move (d)), type (move (t)), value (move (v)) {}

    name (string p, dir_path d, string t, string v)
        : proj (project_name (move (p))), dir (move (d)), type (move (t)),
          value (move (v)) {}

    name (optional<project_name> p, dir_path d, string t, string v)
        : proj (move (p)), dir (move (d)), type (move (t)), value (move (v)) {}

    name (optional<project_name> p,
          dir_path d,
          string t,
          string v,
          optional<pattern_type> pt)
        : proj (move (p)), dir (move (d)), type (move (t)), value (move (v)),
          pattern (pt) {}

    bool
    qualified () const {return proj.has_value ();}

    bool
    unqualified () const {return !qualified ();}

    bool
    typed () const {return !type.empty ();}

    bool
    untyped () const {return type.empty ();}

    // Note: if dir and value are empty then there should be no proj or type.
    //
    bool
    empty () const {return dir.empty () && value.empty ();}

    // Note that strictly speaking the following tests should be orthogonal
    // to qualification. However, the vast majority of cases where we expect
    // a simple or directory name, we also expect it to be unqualified.
    //
    // Note also that empty name is simple but not a directory.
    //
    bool
    simple (bool ignore_qual = false) const
    {
      return (ignore_qual || unqualified ()) && untyped () && dir.empty ();
    }

    bool
    directory (bool ignore_qual = false) const
    {
      return (ignore_qual || unqualified ()) &&
        untyped () && !dir.empty () && value.empty ();
    }

    // File path-like (only optional directory and non-empty value).
    //
    bool
    file (bool ignore_qual = false) const
    {
      return (ignore_qual || unqualified ()) && untyped () && !value.empty ();
    }

    bool
    absolute () const
    {
      return !dir.empty () && dir.absolute ();
    }

    bool
    relative () const
    {
      return dir.empty () || dir.relative ();
    }

    int
    compare (const name&) const;

    // Canonicalize the name by moving the directory component (if any) from
    // value to dir. Throw invalid_argument if value would become empty. May
    // also throw invalid_path.
    //
    void
    canonicalize ();
  };

  LIBBUILD2_SYMEXPORT extern const name empty_name;

  inline bool
  operator== (const name& x, const name& y) {return x.compare (y) == 0;}

  inline bool
  operator!= (const name& x, const name& y) {return !(x == y);}

  inline bool
  operator< (const name& x, const name& y) {return x.compare (y) < 0;}

  // Return string representation of a name.
  //
  // Note that this function does not quote special characters and you should
  // use the to_stream() function below if this is necessary. It also cannot
  // be used on a name pattern.
  //
  LIBBUILD2_SYMEXPORT string
  to_string (const name&);

  template <typename T>
  inline void
  to_checksum (T& cs, const name& n)
  {
    if (n.proj)
      cs.append (n.proj->string ());
    cs.append (n.dir.string ());
    cs.append (n.type);
    cs.append (n.value);
    cs.append (n.pair);
    if (n.pattern)
      cs.append (static_cast<uint8_t> (*n.pattern));
  }

  // Store a string in a name in a reversible way. If the string ends with a
  // trailing directory separator then it is stored as a directory, otherwise
  // as a simple name. Note that the returned name is never a pattern.
  //
  // NOTE: this function does not parse the full name syntax. See context-less
  //       parser::parse_names() for a heavy-weight way to achieve this.
  //
  name
  to_name (string);

  // Serialize the name to the stream. If requested, the name components
  // containing special characters are quoted and/or escaped. In the normal
  // quoting mode the special characters are:
  //
  // {}[]$() \t\n#\"'%
  //
  // And additionally, unless name is a pattern:
  //
  // *?
  //
  // As well as leading and if followed by a non-alphanumeric delimiter:
  //
  // ~^
  //
  // As well as leading `+` if in the curly braces.
  //
  // In the effective quoting mode the special characters are:
  //
  // {}$( \t\n#"'
  //
  // As well as `\` if followed by any of the above characters or itself.
  //
  // If the pair argument is not '\0', then it is added to the above special
  // characters sets. If the quote character is present in the component then
  // it is double quoted rather than single quoted. In this case the following
  // characters are escaped:
  //
  // \$("
  //
  // If escape is true, then escape (with a backslash) the quote characters
  // being added (this is useful if the result will be re-parsed, for example
  // as a Testscript command line).
  //
  // Note that in the quoted mode empty unqualified name is printed as '',
  // not {}.
  //
  enum class quote_mode
  {
    none,
    normal,
    effective
  };

  LIBBUILD2_SYMEXPORT ostream&
  to_stream (ostream&,
             const name&,
             quote_mode,
             char pair = '\0',
             bool escape = false);

  inline ostream&
  operator<< (ostream& os, const name& n) {
    return to_stream (os, n, quote_mode::none);}

  // Vector of names.
  //
  // Quite often it will contain just one element so we use small_vector<1>.
  // Note also that it must be a separate type rather than an alias for
  // vector<name> in order to distinguish between untyped variable values
  // (names) and typed ones (vector<name>).
  //
  using names = small_vector<name, 1>;
  using names_view = vector_view<const name>;

  LIBBUILD2_SYMEXPORT extern const names empty_names;

  // The same semantics as to_stream(name).
  //
  LIBBUILD2_SYMEXPORT ostream&
  to_stream (ostream&,
             const names_view&,
             quote_mode,
             char pair = '\0',
             bool escape = false);

  inline ostream&
  operator<< (ostream& os, const names_view& ns) {
    return to_stream (os, ns, quote_mode::none);}

  inline ostream&
  operator<< (ostream& os, const names& ns) {return os << names_view (ns);}

  // Pair of names.
  //
  using name_pair = pair<name, name>;
}

#include <libbuild2/name.ixx>

#endif // LIBBUILD2_NAME_HXX
