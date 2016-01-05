// file      : build2/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/diagnostics>

#include <cstring>  // strchr()
#include <iostream>

#include <build2/utility>

using namespace std;

namespace build2
{
  // Relative stream.
  //
  const int relative_index = ostream::xalloc ();

  void
  print_process (const char* const* args, size_t n)
  {
    diag_record r (text);
    print_process (r, args, n);
  }

  void
  print_process (diag_record& r, const char* const* args, size_t n)
  {
    size_t m (0);
    const char* const* p (args);
    do
    {
      if (m != 0)
        r << " |"; // Trailing space will be added inside the loop.

      for (m++; *p != nullptr; p++, m++)
      {
        if (p != args)
          r << ' ';

        // Quote if empty or contains spaces.
        //
        bool q (**p == '\0' || strchr (*p, ' ') != nullptr);

        if (q)
          r << '"';

        r << *p;

        if (q)
          r << '"';
      }

      if (m < n) // Can we examine the next element?
      {
        p++;
        m++;
      }

    } while (*p != nullptr);
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
      *diag_stream << os_.str () << std::endl;

      if (epilogue_ != nullptr)
        epilogue_ (*this); // Can throw.
    }
  }

  // Diagnostic facility, project specifics.
  //

  void simple_prologue_base::
  operator() (const diag_record& r) const
  {
    relative (r.os_, relative_);

    if (type_ != nullptr)
      r << type_ << ": ";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  void location_prologue_base::
  operator() (const diag_record& r) const
  {
    relative (r.os_, relative_);

    r << loc_.file << ':' << loc_.line << ':' << loc_.column << ": ";

    if (type_ != nullptr)
      r << type_ << ": ";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  const basic_mark error ("error");
  const basic_mark warn ("warning");
  const basic_mark info ("info");
  const text_mark text;
  const fail_mark<failed> fail;
}
