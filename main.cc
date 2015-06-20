#include "server.h"

#include <string>
#include <iostream>

int main(int argc, char *argv[]) {
  try {
    OSATS::Server s(atoi(argv[1]));
  } catch (std::string err) {
    std::cerr << err << std::endl;
  }
  return 0;
}