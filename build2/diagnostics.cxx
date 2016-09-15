// file      : build2/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/diagnostics>

#include <cstring>  // strchr()
#include <iostream>

using namespace std;

namespace build2
{
  // Stream verbosity.
  //
  const int stream_verb_index = ostream::xalloc ();

  void
  print_process (const char* const* args, size_t n)
  {
    diag_record r (text);
    print_process (r, args, n);
  }

  struct process_args
  {
    const char* const* a;
    size_t n;
  };

  inline static ostream&
  operator<< (ostream& o, const process_args& p)
  {
    process::print (o, p.a, p.n);
    return o;
  }

  void
  print_process (diag_record& r, const char* const* args, size_t n)
  {
    r << process_args {args, n};
  }

  // Diagnostics verbosity level.
  //
  uint16_t verb;

  // Diagnostic facility, base infrastructure.
  //
  ostream* diag_stream = &cerr;

  diag_record::
  ~diag_record () noexcept(false)
  {
    // Don't flush the record if this destructor was called as part of
    // the stack unwinding. Right now this means we cannot use this
    // mechanism in destructors, which is not a big deal, except for
    // one place: exception_guard. So for now we are going to have
    // this ugly special check which we will be able to get rid of
    // once C++17 uncaught_exceptions() becomes available.
    //
    if (!empty_ && (!std::uncaught_exception () || exception_unwinding_dtor))
    {
      *diag_stream << os_.str () << endl;

      if (epilogue_ != nullptr)
        epilogue_ (*this); // Can throw.
    }
  }

  // Diagnostic facility, project specifics.
  //

  void simple_prologue_base::
  operator() (const diag_record& r) const
  {
    stream_verb (r.os_, sverb_);

    if (type_ != nullptr)
      r << type_ << ": ";

    if (mod_ != nullptr)
      r << mod_ << "::";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  void location_prologue_base::
  operator() (const diag_record& r) const
  {
    stream_verb (r.os_, sverb_);

    r << *loc_.file << ':';

    if (loc_.line != 0)
      r << loc_.line << ':';

    if (loc_.column != 0)
      r << loc_.column << ':';

    r << ' ';

    if (type_ != nullptr)
      r << type_ << ": ";

    if (mod_ != nullptr)
      r << mod_ << "::";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  const basic_mark error (&stream_verb_map, "error");
  const basic_mark warn  (&stream_verb_map, "warning");
  const basic_mark info  (&stream_verb_map, "info");
  const basic_mark text  (&stream_verb_map, nullptr);

  const fail_mark<failed> fail;
}
