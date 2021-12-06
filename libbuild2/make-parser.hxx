// file      : libbuild2/make-parser.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_MAKE_PARSER_HXX
#define LIBBUILD2_MAKE_PARSER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Make dependency declaration parser.
  //
  // The format is line-based (but with potential line continuations) so we
  // parse one line at a time. This allows the caller to bail out early (for
  // example, on encountering a non-existent generated file).
  //
  class LIBBUILD2_SYMEXPORT make_parser
  {
  public:
    enum {begin, targets, prereqs, end} state = begin;

    // Parse next target/prerequisite on a line starting from the specified
    // position. Update the position to point to the start of the following
    // target/prerequisite or line.size() if there is nothing left on this
    // line. May return an empty path for a valid if unlikely dependency
    // declarations (see below) or if passing leading blank lines (both of
    // which should normally be just skipped). Issue diagnostics and throw
    // failed if the declaration or path is invalid.
    //
    // If strict is false, then allow unescaped `:` in prerequisites.
    //
    // Note that the (pos != line.size) should be in the do-while rather than
    // in a while loop. In other words, except for the leading blank lines,
    // the parser needs to see the blank line to correctly identify the end of
    // the declaration. See make-parser.test.cxx for a recommended usage.
    //
    // To parse more than one declaration, reset the state to begin after
    // reaching end.
    //
    enum class type {target, prereq};

    pair<type, path>
    next (const string& line, size_t& pos, const location&, bool strict);

    // Lower-level stateless API.
    //
  public:
    // Parse next target/prerequisite on a line starting from the specified
    // position. Return the target/prerequisite as well as an indication of
    // whether the end of the dependency declaration was reached. Update the
    // position to point to the start of the following target/prerequisite,
    // `:`, or line.size() if there is nothing left on this line.
    //
    // Note that some broken tools (notably MinGW GCC) do not escape `:`
    // properly. To tolerate such cases the caller may specify that what's
    // being parsed is the prerequisite list in which case unescaped `:` will
    // be treated literally.
    //
    // Note also that this function may return an empty string (with
    // end=false) for a valid if unlikely dependency declaration, for example
    // (using | to represent backslash):
    //
    // foo:|
    // |
    // bar
    //
    // It would also return an empty string (with end=true) if passed and
    // empty or whitespace-only line.
    //
    // Note also that in the make language line continuations introduce a
    // whitespace rather than just being remove. For example, the following
    // declaration has two prerequisites:
    //
    // foo: bar|
    // baz
    //
    static pair<string, bool>
    next (const string& line, size_t& pos, optional<bool> prereq = nullopt);
  };
}

#endif // LIBBUILD2_MAKE_PARSER_HXX
