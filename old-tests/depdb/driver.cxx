// file      : tests/depdb/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#include <libbutl/filesystem.hxx>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/depdb.hxx>

using namespace std;
using namespace build2;

int
main (int argc, char* argv[])
{
  if (argc != 2)
  {
    cerr << "usage: " << argv[0] << " <db-file>" << endl;
    return 1;
  }

  path p (argv[1]);
  auto rm = [&p] () {try_rmfile (p);};

  // Create empty database.
  //
  {
    rm ();

    {
      depdb d (p);
      assert (d.writing ());
      d.close ();
    }

    {
      depdb d (p);
      assert (d.reading ());
      assert (!d.more ());
      assert (d.read () == nullptr);
      d.close ();
    }
  }

  // No close/end marker.
  //
  {
    rm ();

    {
      depdb d (p);
      assert (d.writing ());
      // No close.
    }

    {
      depdb d (p);
      assert (d.writing ());
      d.close ();
    }

    {
      depdb d (p);
      assert (d.reading ());
    }
  }

  // Overwrite/append/truncate.
  //
  {
    rm ();

    {
      depdb d (p);
      d.write ("foo");
      d.close ();
    }

    {
      depdb d (p);
      assert (*d.read () == "foo");
      assert (!d.more ());
      d.close ();
    }

    {
      depdb d (p);
      assert (*d.read () == "foo");
      assert (!d.more ());
      d.write ("FOO");
      d.close ();
    }

    {
      depdb d (p);
      assert (*d.read () == "FOO");
      assert (!d.more ());
      assert (d.read () == nullptr);
      assert (d.read () == nullptr);
      d.write ("BAR");
      d.close ();
    }

    {
      depdb d (p);
      assert (*d.read () == "FOO");
      assert (d.more ());
      d.write ("foo");
      d.close (); // Truncate.
    }

    {
      depdb d (p);
      assert (*d.read () == "foo");
      assert (!d.more ());
    }

    // Stray end marker.
    //
    {
      depdb d (p);
      assert (*d.read () == "foo");
      d.write ("fox");
      // No close.
    }

    {
      depdb d (p);
      assert (d.more ());
      assert (*d.read () == "fox");
      assert (!d.more ());
    }
  }

  // Read/truncate.
  //
  {
    rm ();

    {
      depdb d (p);
      d.write ("foo");
      d.write ("bar");
      d.close ();
    }

    {
      depdb d (p);
      assert (*d.read () == "foo");
      assert (d.more ());
      d.close (); // Truncate bar.
    }

    {
      depdb d (p);
      assert (*d.read () == "foo");
      assert (!d.more ());
    }
  }

  rm ();
}
