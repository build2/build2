#include <iostream>

using namespace std;

int
main (int argc, const char* argv[])
{
  for (int i (1); i < argc; ++i)
    cout << "'" << argv[i] << "'" << endl;
}
