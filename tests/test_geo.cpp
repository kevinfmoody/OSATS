#include "osats/geo.hpp"

#include <algorithm>
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

const char* tierName(osats::geo::InterestTier tier) {
  switch (tier) {
    case osats::geo::InterestTier::Near: return "near";
    case osats::geo::InterestTier::Mid: return "mid";
    case osats::geo::InterestTier::Far: return "far";
    case osats::geo::InterestTier::None: return "none";
  }
  return "?";
}

void expectOffset(const osats::Position& origin, double north_nm, double east_nm,
                  double expect_nm, osats::geo::InterestTier tier) {
  osats::Position p = origin;
  p.latitude_deg += north_nm / 60.0;
  const double cos_lat = std::cos(origin.latitude_deg * M_PI / 180.0);
  p.longitude_deg += east_nm / (60.0 * cos_lat);
  const double d = osats::geo::distanceNm(origin, p);
  const auto got = osats::geo::classifyTier(expect_nm, 50.0);
  if (!(std::abs(d - expect_nm) < 1e-4 && got == tier &&
        osats::geo::classifyTier(std::min(d, expect_nm), 50.0) == tier)) {
    std::cerr << "offset lat=" << origin.latitude_deg << " N=" << north_nm
              << " E=" << east_nm << " got " << d << " nm tier "
              << tierName(osats::geo::classifyTier(d, 50.0)) << " expected "
              << expect_nm << " " << tierName(tier) << "\n";
    std::exit(1);
  }
}

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
  // boundsNm still widens longitude by 1/cos(lat). At 60°N a 60 nm radius
  // is 2° of longitude each side, so the window is about 4° wide.
  Position high{60.0, 0, 0, 0, 0};
  geo::boundsNm(high, 60.0, min_lat, max_lat, min_lon, max_lon);
  CHECK(max_lon - min_lon > 3.5);

  CHECK(geo::classifyTier(5, 50) == geo::InterestTier::Near);
  CHECK(geo::classifyTier(20, 50) == geo::InterestTier::Mid);
  CHECK(geo::classifyTier(40, 50) == geo::InterestTier::Far);
  CHECK(geo::classifyTier(80, 50) == geo::InterestTier::None);
  CHECK(geo::minIntervalMs(geo::InterestTier::Near) == 200);
  CHECK(geo::minIntervalMs(geo::InterestTier::Mid) == 1000);
  CHECK(geo::minIntervalMs(geo::InterestTier::Far) == 5000);

  const Position ksfo{37.62, -122.38, 0, 0, 0};
  const Position north{60.0, 0, 0, 0, 0};
  const struct {
    double nm;
    geo::InterestTier tier;
  } legs[] = {{10.0, geo::InterestTier::Near},
              {30.0, geo::InterestTier::Mid},
              {50.0, geo::InterestTier::Far}};
  for (const auto& leg : legs) {
    expectOffset(ksfo, leg.nm, 0, leg.nm, leg.tier);
    expectOffset(ksfo, 0, leg.nm, leg.nm, leg.tier);
    expectOffset(north, leg.nm, 0, leg.nm, leg.tier);
    expectOffset(north, 0, leg.nm, leg.nm, leg.tier);
  }

  // 1 nm/s north for 1 s is 1/60°, and vd positive is down.
  Position coast{37.62, -122.38, 10000, 0, 0};
  coast.vn_fps = static_cast<float>(geo::kKtToFps * 3600.0);
  coast.vd_fps = 25.f;
  Position stepped = coast;
  geo::extrapolate(stepped, 1.0);
  CHECK(std::abs(stepped.latitude_deg - (37.62 + 1.0 / 60.0)) < 1e-6);
  CHECK(std::abs(stepped.longitude_deg - coast.longitude_deg) < 1e-9);
  CHECK(std::abs(stepped.altitude_ft - 9975.0) < 1e-3);
  geo::extrapolate(coast, 0.0);
  CHECK(std::abs(coast.latitude_deg - 37.62) < 1e-12);

  Position eastward{60.0, 0, 0, 0, 0};
  eastward.ve_fps = static_cast<float>(geo::kKtToFps * 3600.0);
  geo::extrapolate(eastward, 1.0);
  CHECK(std::abs(eastward.longitude_deg - (1.0 / 30.0)) < 1e-5);

  Position held{0, 0, 0, 0, 0};
  held.vn_fps = static_cast<float>(geo::kKtToFps * 3600.0);
  Position capped = held;
  geo::extrapolate(capped, 100.0);
  Position five = held;
  geo::extrapolate(five, geo::kMaxExtrapolateSec);
  CHECK(std::abs(capped.latitude_deg - five.latitude_deg) < 1e-9);

  // Date line: 0.2° of longitude is 12 nm, not a trip the long way around.
  Position east{0, 179.9, 0, 0, 0};
  Position west{0, -179.9, 0, 0, 0};
  CHECK(std::abs(geo::distanceNm(east, west) - 12.0) < 0.05);

  std::cout << "test_geo ok\n";
  return 0;
}
