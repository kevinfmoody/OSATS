#include "osats/spatial_index.hpp"

#include <cstdlib>
#include <iostream>

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr "\n";                              \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

int main() {
  using namespace osats;
  SpatialIndex index(0.25);

  Position p1{37.0, -122.0, 0, 0, 0};
  Position p2{37.01, -122.01, 0, 0, 0};
  Position p3{40.0, -74.0, 0, 0, 0};

  index.upsert(1, p1);
  index.upsert(2, p2);
  index.upsert(3, p3);
  CHECK(index.size() == 3);

  std::vector<EntityId> hits;
  index.queryRadius(p1, 20.0, hits);
  bool found2 = false;
  bool found3 = false;
  for (auto id : hits) {
    if (id == 2) found2 = true;
    if (id == 3) found3 = true;
  }
  CHECK(found2);
  CHECK(!found3);

  index.remove(2);
  CHECK(index.size() == 2);
  index.queryRadius(p1, 20.0, hits);
  for (auto id : hits) CHECK(id != 2);

  std::cout << "test_spatial_index ok\n";
  return 0;
}
