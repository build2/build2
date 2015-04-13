// file      : build/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
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

  // diag_do(), etc.
  //
  string
  diag_do (const action&, const target& t)
  {
    const meta_operation_info& mi (*current_mif);
    const operation_info& oi (*current_oif);

    ostringstream os;

    // perform(update(x))   -> "update x"
    // configure(update(x)) -> "configure updating x"
    //
    if (mi.name_do.empty ())
      os << oi.name_do << ' ';
    else
    {
      os << mi.name_do << ' ';

      if (!oi.name_doing.empty ())
        os << oi.name_doing << ' ';
    }

    os << t;
    return os.str ();
  }

  string
  diag_doing (const action&, const target& t)
  {
    const meta_operation_info& mi (*current_mif);
    const operation_info& oi (*current_oif);

    ostringstream os;

    // perform(update(x))   -> "updating x"
    // configure(update(x)) -> "configuring updating x"
    //
    if (!mi.name_doing.empty ())
      os << mi.name_doing << ' ';

    if (!oi.name_doing.empty ())
      os << oi.name_doing << ' ';

    os << t;
    return os.str ();
  }

  string
  diag_already_done (const action&, const target& t)
  {
    const meta_operation_info& mi (*current_mif);
    const operation_info& oi (*current_oif);

    ostringstream os;

    // perform(update(x))   -> "x is already up to date"
    // configure(update(x)) -> "updating x is already configured"
    //
    if (mi.name_already_done.empty ())
    {
      os << t;

      if (!oi.name_already_done.empty ())
        os << " is already " << oi.name_already_done;
    }
    else
    {
      if (!oi.name_doing.empty ())
        os << oi.name_doing << ' ';

      os << t << " is already " << mi.name_already_done;
    }

    return os.str ();
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
