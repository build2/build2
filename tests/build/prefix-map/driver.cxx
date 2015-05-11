// file      : tests/build/prefix-map/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <cassert>
#include <iostream>

#include <build/prefix-map>

using namespace std;
using namespace build;

int
main ()
{
  typedef prefix_map<string, int, '.'> pm;

  {
    const pm m;

    {
      auto r (m.find_prefix (""));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("foo"));
      assert (r.first == r.second);
    }
  }

  {
    pm m {{{"foo", 1}}};

    {
      auto r (m.find_prefix (""));
      assert (r.first != r.second && r.first->second == 1 &&
              ++r.first == r.second);
    }

    {
      auto r (m.find_prefix ("fo"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("fox"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("fooo"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("foo.bar"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("foo"));
      assert (r.first != r.second && r.first->second == 1 &&
              ++r.first == r.second);
    }
  }

  {
    pm m {{{"foo", 1}, {"bar", 2}}};

    {
      auto r (m.find_prefix (""));
      assert (r.first != r.second && r.first->second == 2 &&
              ++r.first != r.second && r.first->second == 1 &&
              ++r.first == r.second);
    }

    {
      auto r (m.find_prefix ("fo"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("fox"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("fooo"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("foo.bar"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("foo"));
      assert (r.first != r.second && r.first->second == 1 &&
              ++r.first == r.second);
    }

    {
      auto r (m.find_prefix ("bar"));
      assert (r.first != r.second && r.first->second == 2 &&
              ++r.first == r.second);
    }
  }

  {
    pm m (
      {{"boo", 1},
       {"foo", 2}, {"fooa", 3}, {"foo.bar", 4}, {"foo.fox", 5},
       {"xoo", 5}});

    {
      auto r (m.find_prefix ("fo"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("fox"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("fooo"));
      assert (r.first == r.second);
    }

    {
      auto r (m.find_prefix ("foo.bar"));
      assert (r.first != r.second && r.first->second == 4 &&
              ++r.first == r.second);
    }

    {
      auto r (m.find_prefix ("foo.fox"));
      assert (r.first != r.second && r.first->second == 5 &&
              ++r.first == r.second);
    }

    {
      auto r (m.find_prefix ("foo"));
      assert (r.first != r.second && r.first->second == 2 &&
              ++r.first != r.second && r.first->second == 4 &&
              ++r.first != r.second && r.first->second == 5 &&
              ++r.first == r.second);
    }
  }
}
