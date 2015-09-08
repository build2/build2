// file      : build/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/diagnostics>

#include <sstream>
#include <iostream>

#include <build/scope>
#include <build/target>
#include <build/operation>
#include <build/context>
#include <build/utility>

using namespace std;

namespace build
{
  string
  diag_relative (const path& p)
  {
    const path& b (*relative_base);

    if (p.absolute ())
    {
      if (p == b)
        return ".";

#ifndef _WIN32
      if (p == home)
        return "~";
#endif

      path rb (relative (p));

#ifndef _WIN32
      if (rb.relative ())
      {
        // See if the original path with the ~/ shortcut is better
        // that the relative to base.
        //
        if (p.sub (home))
        {
          path rh (p.leaf (home));
          if (rb.string ().size () > rh.string ().size () + 2) // 2 for '~/'
            return "~/" + rh.string ();
        }
      }
      else if (rb.sub (home))
        return "~/" + rb.leaf (home).string ();
#endif

      return rb.string ();
    }

    return p.string ();
  }

  string
  diag_relative (const dir_path& d, bool cur)
  {
    string r (diag_relative (static_cast<const path&> (d)));

    // Translate "." to empty.
    //
    if (!cur && d.absolute () && r == ".")
      r.clear ();

    // Add trailing '/'.
    //
    if (!r.empty () && !dir_path::traits::is_separator (r.back ()))
      r += '/';

    return r;
  }

  // Relative stream.
  //
  const int relative_index = ostream::xalloc ();

  // diag_do(), etc.
  //
  string
  diag_do (const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    ostringstream os;

    // perform(update(x))   -> "update x"
    // configure(update(x)) -> "configure updating x"
    //
    if (m.name_do.empty ())
      os << io.name_do << ' ';
    else
    {
      os << m.name_do << ' ';

      if (!io.name_doing.empty ())
        os << io.name_doing << ' ';
    }

    if (oo != nullptr)
      os << "(for " << oo->name << ") ";

    os << t;
    return os.str ();
  }

  string
  diag_doing (const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    ostringstream os;

    // perform(update(x))   -> "updating x"
    // configure(update(x)) -> "configuring updating x"
    //
    if (!m.name_doing.empty ())
      os << m.name_doing << ' ';

    if (!io.name_doing.empty ())
      os << io.name_doing << ' ';

    if (oo != nullptr)
      os << "(for " << oo->name << ") ";

    os << t;
    return os.str ();
  }

  string
  diag_done (const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    ostringstream os;

    // perform(update(x))   -> "x is up to date"
    // configure(update(x)) -> "updating x is configured"
    //
    if (m.name_done.empty ())
    {
      os << t;

      if (!io.name_done.empty ())
        os << " " << io.name_done;

      if (oo != nullptr)
        os << "(for " << oo->name << ") ";
    }
    else
    {
      if (!io.name_doing.empty ())
        os << io.name_doing << ' ';

      if (oo != nullptr)
        os << "(for " << oo->name << ") ";

      os << t << " " << m.name_done;
    }

    return os.str ();
  }

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
        r << (p != args ? " " : "")
          << (**p == '\0' ? "\"" : "") // Quote empty arguments.
          << *p
          << (**p == '\0' ? "\"" : "");

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
