// file      : build2/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/diagnostics.hxx>

#include <cstring>  // strchr()

#include <libbutl/process-io.mxx>

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

  void
  print_process (diag_record& r, const char* const* args, size_t n)
  {
    r << butl::process_args {args, n};
  }

  // Diagnostics verbosity level.
  //
  uint16_t verb = 0; // Keep disabled until set from options.

  // Diagnostics stack.
  //
#ifdef __cpp_thread_local
  thread_local
#else
  __thread
#endif
  const diag_frame* diag_frame::stack = nullptr;

  // Diagnostic facility, project specifics.
  //

  void simple_prologue_base::
  operator() (const diag_record& r) const
  {
    stream_verb (r.os, sverb_);

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
    stream_verb (r.os, sverb_);

    if (!loc_.empty ())
    {
      r << *loc_.file << ':';

      if (!ops.no_line ())
      {
        if (loc_.line != 0)
          r << loc_.line << ':';

        if (!ops.no_column ())
        {
          if (loc_.column != 0)
            r << loc_.column << ':';
        }
      }

      r << ' ';
    }

    if (type_ != nullptr)
      r << type_ << ": ";

    if (mod_ != nullptr)
      r << mod_ << "::";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  const basic_mark error ("error");
  const basic_mark warn  ("warning");
  const basic_mark info  ("info");
  const basic_mark text  (nullptr, nullptr, nullptr); // No type/data/frame.
  const fail_mark  fail  ("error");
  const fail_end   endf;
}
