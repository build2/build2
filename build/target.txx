// file      : build/target.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/scope>
#include <build/context>     // extension_pool
#include <build/diagnostics>
#include <build/prerequisite>

namespace build
{
  template <const char* ext>
  const string&
  target_extension_fix (const target_key&, scope&)
  {
    return extension_pool.find (ext);
  }

  template <const char* var, const char* def>
  const string&
  target_extension_var (const target_key& tk, scope& s)
  {
    // Include target type/pattern-specific variables.
    //
    if (auto l = s.lookup (tk, var))
      return extension_pool.find (as<string> (*l));

    if (def != nullptr)
      return extension_pool.find (def);

    {
      diag_record dr;
      dr << error << "no default extension in variable '" << var << "'"
         << info << "required to derive file name for ";

      // This is a bit hacky: we may be dealing with a target (see
      // file::derive_path()) or prerequsite (see search_existing_file()).
      // So we are going to check if dir is absolute. If it is, then
      // we assume this is a target, otherwise -- prerequsite.
      //
      if (tk.dir->absolute ())
        dr << "target " << tk;
      else
      {
        const string* proj (nullptr); // Used for local prerequisites.
        dr << "prerequisite " << prerequisite_key {&proj, tk, &s};
      }

      dr << info << "perhaps you forgot to add "
         << tk.type->name << "{*}: " << var << " = ...";
    }

    throw failed ();
  }
}
