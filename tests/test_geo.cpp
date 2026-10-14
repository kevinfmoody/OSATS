#include "osats/geo.hpp"

#include <cmath>
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
  Position a{0, 0, 0, 0, 0};
  Position b{0, 1.0 / 60.0, 0, 0, 0};  // ~1 nm east at equator
  const double d = geo::distanceNm(a, b);
  CHECK(std::abs(d - 1.0) < 0.05);

  double min_lat, max_lat, min_lon, max_lon;
  geo::boundsNm(a, 60.0, min_lat, max_lat, min_lon, max_lon);
  CHECK(min_lat < 0 && max_lat > 0);
  CHECK(min_lon < 0 && max_lon > 0);

  CHECK(geo::classifyTier(5, 50) == geo::InterestTier::Near);
  CHECK(geo::classifyTier(20, 50) == geo::InterestTier::Mid);
  CHECK(geo::classifyTier(40, 50) == geo::InterestTier::Far);
  CHECK(geo::classifyTier(80, 50) == geo::InterestTier::None);
  CHECK(geo::minIntervalMs(geo::InterestTier::Near) == 200);

  std::cout << "test_geo ok\n";
  return 0;
}
