#pragma once

#include "osats/types.hpp"

#include <algorithm>
#include <cmath>

namespace osats::geo {

inline constexpr double kNmPerDegreeLat = 60.0;
// 1 knot in feet per second. Shared by the CLI motion model and extrapolation.
inline constexpr double kKtToFps = 1.68781;
// Coast at most one far-tier interval past the last sample.
inline constexpr double kMaxExtrapolateSec = 5.0;
// Pilots and observers stay near the default 50 nm bubble. Controllers may
// look farther. kMaxVisibilityNm is the widest interest query.
inline constexpr double kPilotVisibilityNm = 50.0;
inline constexpr double kControllerVisibilityNm = 100.0;
inline constexpr double kMaxVisibilityNm = kControllerVisibilityNm;

inline double visibilityCapNm(Role role) {
  return role == Role::Controller ? kControllerVisibilityNm : kPilotVisibilityNm;
}

inline double degToRad(double deg) { return deg * M_PI / 180.0; }
inline double radToDeg(double rad) { return rad * 180.0 / M_PI; }
inline double nmToDegLat(double nm) { return nm / kNmPerDegreeLat; }

inline void normalizeLon(double& lon) {
  while (lon < -180.0) lon += 360.0;
  while (lon > 180.0) lon -= 360.0;
}

// cos(lat), floored so the poles stay finite. Longitude degrees × cos(lat)
// are nautical miles on the same scale as latitude degrees.
inline double cosLat(double lat_deg) {
  const double c = std::cos(degToRad(std::clamp(lat_deg, -89.0, 89.0)));
  return c > 1e-6 ? c : 1e-6;
}

// Degrees of longitude per degree of latitude. boundsNm uses this to widen
// the search window; distance uses the inverse (cosLat).
inline double lonScaleAt(double lat_deg) { return 1.0 / cosLat(lat_deg); }

// Equirectangular distance. Wrap the raw longitude delta before scaling so
// the date line stays a short hop. 1° latitude = 60 nm.
inline double distanceNmSquared(const Position& a, const Position& b) {
  const double scale = 0.5 * (cosLat(a.latitude_deg) + cosLat(b.latitude_deg));
  double dlat = a.latitude_deg - b.latitude_deg;
  double dlon = a.longitude_deg - b.longitude_deg;
  if (dlon > 180.0) dlon -= 360.0;
  if (dlon < -180.0) dlon += 360.0;
  dlon *= scale;
  const double deg2 = dlat * dlat + dlon * dlon;
  return deg2 * (kNmPerDegreeLat * kNmPerDegreeLat);
}

inline double distanceNm(const Position& a, const Position& b) {
  return std::sqrt(distanceNmSquared(a, b));
}

// Advance a sample by its N/E/D velocity. vd is positive down. dt is clamped
// so a quiet target does not keep flying after the far-tier ceiling.
inline void extrapolate(Position& pos, double dt_sec) {
  if (!(dt_sec > 0.0)) return;
  if (dt_sec > kMaxExtrapolateSec) dt_sec = kMaxExtrapolateSec;
  const double ft_per_nm = kKtToFps * 3600.0;
  const double lat = pos.latitude_deg;
  pos.latitude_deg += (static_cast<double>(pos.vn_fps) * dt_sec / ft_per_nm) / kNmPerDegreeLat;
  pos.longitude_deg +=
      (static_cast<double>(pos.ve_fps) * dt_sec / ft_per_nm) / (kNmPerDegreeLat * cosLat(lat));
  normalizeLon(pos.longitude_deg);
  pos.altitude_ft -= static_cast<double>(pos.vd_fps) * dt_sec;
}

inline void boundsNm(const Position& pos, double radius_nm,
                     double& min_lat, double& max_lat,
                     double& min_lon, double& max_lon) {
  const double range = nmToDegLat(radius_nm);
  min_lat = pos.latitude_deg - range;
  max_lat = pos.latitude_deg + range;
  const double scale = std::max(lonScaleAt(min_lat), lonScaleAt(max_lat));
  min_lon = pos.longitude_deg - scale * range;
  max_lon = pos.longitude_deg + scale * range;
  normalizeLon(min_lon);
  normalizeLon(max_lon);
}

// Interest tiers by distance (nm). Near peers get higher update rates.
enum class InterestTier : std::uint8_t {
  Near = 0,   // <= 10 nm
  Mid = 1,    // <= 30 nm
  Far = 2,    // <= visibility
  None = 3,
};

inline InterestTier classifyTier(double distance_nm, double visibility_nm) {
  if (distance_nm > visibility_nm) return InterestTier::None;
  if (distance_nm <= 10.0) return InterestTier::Near;
  if (distance_nm <= 30.0) return InterestTier::Mid;
  return InterestTier::Far;
}

inline std::uint32_t minIntervalMs(InterestTier tier) {
  switch (tier) {
    case InterestTier::Near: return 200;   // 5 Hz
    case InterestTier::Mid: return 1000;   // 1 Hz
    case InterestTier::Far: return 5000;   // 0.2 Hz
    case InterestTier::None: return 0;
  }
  return 0;
}

}  // namespace osats::geo
