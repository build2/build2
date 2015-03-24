// file      : build/file.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/file>

#include <fstream>

#include <build/scope>
#include <build/parser>
#include <build/filesystem>
#include <build/diagnostics>

using namespace std;

namespace build
{
  void
  source (const path& bf, scope& root, scope& base)
  {
    tracer trace ("source");

    ifstream ifs (bf.string ());
    if (!ifs.is_open ())
      fail << "unable to open " << bf;

    level4 ([&]{trace << "sourcing " << bf;});

    ifs.exceptions (ifstream::failbit | ifstream::badbit);
    parser p;

    try
    {
      p.parse_buildfile (ifs, bf, root, base);
    }
    catch (const std::ios_base::failure&)
    {
      fail << "failed to read from " << bf;
    }
  }

  void
  source_once (const path& bf, scope& root, scope& base, scope& once)
  {
    tracer trace ("source_once");

    if (!once.buildfiles.insert (bf).second)
    {
      level4 ([&]{trace << "skipping already sourced " << bf;});
      return;
    }

    source (bf, root, base);
  }

  void
  root_pre (scope& root)
  {
    tracer trace ("root_pre");

    path bf (root.src_path () / path ("build/root.build"));

    if (!file_exists (bf))
      return;

    source_once (bf, root, root);
  }
}
