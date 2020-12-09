// file      : libbuild2/script/timeout.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/script/timeout.hxx>

#include <chrono>

#include <libbuild2/diagnostics.hxx>

using namespace std;

namespace build2
{
  optional<duration>
  parse_timeout (const string& s,
                 const char* what,
                 const char* prefix,
                 const location& l)
  {
    if (optional<uint64_t> n = parse_number (s))
    {
      return *n != 0
        ? chrono::duration_cast<duration> (chrono::seconds (*n))
        : optional<duration> ();
    }
    else
      fail (l) << prefix << "invalid " << what << " '" << s << "'" << endf;
  }
}
