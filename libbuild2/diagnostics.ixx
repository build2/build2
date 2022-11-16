// file      : libbuild2/diagnostics.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  LIBBUILD2_SYMEXPORT void
  print_diag_impl (const char*, target_key*, target_key&&, const char*);

  inline void
  print_diag (const char* p, target_key&& l, target_key&& r, const char* c)
  {
    print_diag_impl (p, &l, move (r), c);
  }

  inline void
  print_diag (const char* p, target_key& r)
  {
    print_diag_impl (p, nullptr, move (r), nullptr);
  }

  inline void
  print_diag (const char* p, const path& r)
  {
    print_diag (p, path_name (&r));
  }

  inline void
  print_diag (const char* p, const target& l, const path& r, const char* c)
  {
    print_diag (p, l, path_name (&r), c);
  }

  inline void
  print_diag (const char* p, const path& l, const path& r, const char* c)
  {
    print_diag (p, l, path_name (&r), c);
  }

  inline void
  print_diag (const char* p, const string& l, const path& r, const char* c)
  {
    print_diag (p, l, path_name (&r), c);
  }
}
