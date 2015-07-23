#include <iostream>
#include <cassert>

using namespace std;

int
main ()
{
  cerr << "test is running (stderr)" << endl;
  //assert (false);
  cout << "test is running (stdout)" << endl;
  return 0;
  //return 1;
}
