// file      : build2/regex.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/regex.hxx>

#if defined(_MSC_VER) && _MSC_VER <= 1910
#  include <cstring> // strstr()
#endif

#include <ostream>
#include <sstream>

namespace std
{
  // Currently libstdc++ just returns the name of the exception (bug #67361).
  // So we check that the description contains at least one space character.
  //
  // While VC's description is meaningful, it has an undesired prefix that
  // resembles the following: 'regex_error(error_badrepeat): '. So we skip it.
  //
  ostream&
  operator<< (ostream& o, const regex_error& e)
  {
    const char* d (e.what ());

#if defined(_MSC_VER) && _MSC_VER <= 1910
    const char* rd (strstr (d, "): "));
    if (rd != nullptr)
      d = rd + 3;
#endif

    ostringstream os;
    os << runtime_error (d); // Sanitize the description.

    string s (os.str ());
    if (s.find (' ') != string::npos)
      o << ": " << s;

    return o;
  }
}
