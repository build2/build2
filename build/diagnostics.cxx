// file      : build/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/diagnostics>

#include <iostream>

#include <build/context>
#include <build/utility>

using namespace std;

namespace build
{
  string
  diag_relative_work (const path& p)
  {
    if (p.absolute ())
    {
      if (p == work)
        return ".";

      path rp (relative_work (p));

#ifndef _WIN32
      if (rp.absolute () && rp.sub (home))
        return "~/" + rp.leaf (home).string ();
#endif

      return rp.string ();
    }

    return p.string ();
  }

  void
  print_process (const char* const* args)
  {
    diag_record r (text);

    for (const char* const* p (args); *p != nullptr; p++)
      r << (p != args ? " " : "") << *p;
  }

  // Trace verbosity level.
  //
  uint8_t verb;

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
    if (type_ != nullptr)
      r << type_ << ": ";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  void location_prologue_base::
  operator() (const diag_record& r) const
  {
    r << loc_.file << ':' << loc_.line << ':' << loc_.column << ": ";

    if (type_ != nullptr)
      r << type_ << ": ";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  const basic_mark error ("error");
  const basic_mark warn ("warning");
  const basic_mark info ("info");
  const basic_mark text (nullptr);

  const fail_mark<failed> fail;
}
