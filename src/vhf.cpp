#include "osats/vhf.hpp"
#include "osats/geo.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace osats::vhf {
namespace {

struct LatLon {
  double lat = 0.0;
  double lon = 0.0;
};

LatLon pointOnTrack(const Antenna& from, const Antenna& to, double t) {
  double dlon = to.longitude_deg - from.longitude_deg;
  if (dlon > 180.0) dlon -= 360.0;
  if (dlon < -180.0) dlon += 360.0;
  LatLon p;
  p.lat = from.latitude_deg + (to.latitude_deg - from.latitude_deg) * t;
  p.lon = from.longitude_deg + dlon * t;
  geo::normalizeLon(p.lon);
  return p;
}

// d_nm = C * sqrt(h_ft) is the 4/3-earth horizon, so the bulge that brings
// the ray down to the surface at that horizon is d1 * d2 / C^2 feet.
double earthBulgeFt(double d1_nm, double d2_nm, double horizon_constant) {
  const double c2 = horizon_constant * horizon_constant;
  return (d1_nm * d2_nm) / c2;
}

int sampleIntervals(double distance_nm, double spacing_nm) {
  const double spacing = spacing_nm > 0.01 ? spacing_nm : 0.01;
  double steps = std::ceil(distance_nm / spacing);
  if (!(steps > 1.0)) steps = 2.0;
  if (steps > 10000.0) steps = 10000.0;
  return static_cast<int>(steps);
}

double clearanceFactor(double fraction, double well_clear_fraction) {
  if (!(fraction > 0.0)) return 0.0;
  if (!(well_clear_fraction > 0.0)) return 1.0;
  return std::clamp(fraction / well_clear_fraction, 0.0, 1.0);
}

void applySquelch(Path& path, double raw, double squelch) {
  path.raw_coupling = std::clamp(raw, 0.0, 1.0);
  path.audible = path.same_frequency && path.within_horizon && path.terrain_clear &&
                 path.raw_coupling >= squelch;
  path.coupling = path.audible ? path.raw_coupling : 0.0;
}

std::optional<Antenna> resolve(const AircraftState& station, const FacilityTable& facilities,
                               const ElevationSampler& terrain, const Params& params) {
  if (station.role == Role::Controller) {
    const FacilitySite* site = facilities.find(station.callsign);
    if (!site) return std::nullopt;
    return placeFacilityAntenna(*site, station.radio_hz);
  }
  return placeAircraftAntenna(station.position, station.radio_hz, terrain, params);
}

AircraftState stationFrom(const Transceiver& tuned, EntityId id) {
  AircraftState state;
  state.id = id;
  state.callsign = tuned.callsign;
  state.role = tuned.role;
  state.radio_hz = tuned.frequency_khz;
  state.position.latitude_deg = tuned.latitude_deg;
  state.position.longitude_deg = tuned.longitude_deg;
  state.position.altitude_ft = tuned.altitude_ft;
  return state;
}

void noteFacility(FacilityTable& facilities, const Transceiver& tuned) {
  if (tuned.role == Role::Controller && tuned.facility != nullptr) {
    facilities.upsert(tuned.callsign, *tuned.facility);
  }
}

}  // namespace

Antenna placeAircraftAntenna(const Position& position, std::uint32_t frequency_khz,
                             const ElevationSampler& terrain, const Params& params) {
  const double ground =
      terrain.elevationMslFt(position.latitude_deg, position.longitude_deg);
  const double floor_ft = params.surface_floor_ft > 0.0 ? params.surface_floor_ft : 0.0;
  const double agl = std::max(floor_ft, position.altitude_ft - ground);
  Antenna ant;
  ant.latitude_deg = position.latitude_deg;
  ant.longitude_deg = position.longitude_deg;
  ant.height_agl_ft = agl;
  ant.altitude_msl_ft = ground + agl;
  ant.frequency_khz = frequency_khz;
  return ant;
}

Antenna placeFacilityAntenna(const FacilitySite& site, std::uint32_t frequency_khz) {
  const double mast = site.mast_height_ft > 0.0 ? site.mast_height_ft : 0.0;
  Antenna ant;
  ant.latitude_deg = site.latitude_deg;
  ant.longitude_deg = site.longitude_deg;
  ant.height_agl_ft = mast;
  ant.altitude_msl_ft = site.field_elevation_ft + mast;
  ant.frequency_khz = frequency_khz;
  return ant;
}

Path evaluate(const Antenna& tx, const Antenna& rx, const ElevationSampler& terrain,
              const Params& params) {
  Path path;
  path.same_frequency = tx.frequency_khz == rx.frequency_khz;
  if (!path.same_frequency) return path;

  const double h_tx = tx.height_agl_ft > 0.0 ? tx.height_agl_ft : 0.0;
  const double h_rx = rx.height_agl_ft > 0.0 ? rx.height_agl_ft : 0.0;
  path.horizon_nm = radioHorizonNm(h_tx, h_rx, params.horizon_constant);

  Position a;
  Position b;
  a.latitude_deg = tx.latitude_deg;
  a.longitude_deg = tx.longitude_deg;
  b.latitude_deg = rx.latitude_deg;
  b.longitude_deg = rx.longitude_deg;
  path.distance_nm = geo::distanceNm(a, b);

  if (!(path.horizon_nm > 0.0) || !(path.distance_nm < path.horizon_nm)) {
    return path;
  }
  path.within_horizon = true;

  // Coincident antennas: nothing between them to occlude.
  if (path.distance_nm < 1e-4) {
    path.terrain_clear = true;
    path.min_clearance_ft = std::min(h_tx, h_rx);
    applySquelch(path, 1.0, params.squelch);
    return path;
  }

  const int intervals = sampleIntervals(path.distance_nm, params.sample_spacing_nm);
  double min_fraction = 1.0;
  double min_clearance = std::numeric_limits<double>::infinity();
  bool blocked = false;

  for (int i = 1; i < intervals; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(intervals);
    const LatLon at = pointOnTrack(tx, rx, t);
    const double ground = terrain.elevationMslFt(at.lat, at.lon);
    const double d1 = t * path.distance_nm;
    const double d2 = path.distance_nm - d1;
    const double bulge = earthBulgeFt(d1, d2, params.horizon_constant);
    const double ray_msl =
        tx.altitude_msl_ft + (rx.altitude_msl_ft - tx.altitude_msl_ft) * t - bulge;
    const double room = h_tx + (h_rx - h_tx) * t;
    const double clearance = ray_msl - ground;
    min_clearance = std::min(min_clearance, clearance);
    if (!(clearance > 0.0) || !(room > 1e-3)) {
      blocked = true;
      continue;
    }
    min_fraction = std::min(min_fraction, clearance / room);
  }

  path.min_clearance_ft = min_clearance;
  if (blocked) return path;

  path.terrain_clear = true;
  // 1 beside the antenna with the ridge well below the ray, falling to 0 at
  // the horizon or as clearance gets thin.
  const double range_factor =
      (path.horizon_nm - path.distance_nm) / path.horizon_nm;
  const double raw = range_factor * clearanceFactor(min_fraction, params.well_clear_fraction);
  applySquelch(path, raw, params.squelch);
  return path;
}

Path propagate(const AircraftState& transmitter, const AircraftState& receiver,
               const FacilityTable& facilities, const ElevationSampler& terrain,
               const Params& params) {
  Path path;
  path.same_frequency = transmitter.radio_hz == receiver.radio_hz;
  // A spawned station does not hear its own transmission.
  if (transmitter.id != kInvalidEntityId && transmitter.id == receiver.id) {
    return path;
  }
  if (!path.same_frequency) return path;

  const auto tx = resolve(transmitter, facilities, terrain, params);
  const auto rx = resolve(receiver, facilities, terrain, params);
  if (!tx || !rx) return path;
  return evaluate(*tx, *rx, terrain, params);
}

std::vector<Heard> hearSet(const Transceiver& transmission,
                           std::span<const Transceiver> candidates,
                           const ElevationSampler& terrain, const Params& params) {
  FacilityTable facilities;
  noteFacility(facilities, transmission);
  const AircraftState tx = stationFrom(transmission, 1);

  std::vector<Heard> heard;
  EntityId next_id = 2;
  for (const Transceiver& candidate : candidates) {
    if (candidate.callsign == transmission.callsign) continue;
    noteFacility(facilities, candidate);
    const Path path =
        propagate(tx, stationFrom(candidate, next_id++), facilities, terrain, params);
    if (!path.audible) continue;
    Heard row;
    row.callsign = candidate.callsign;
    row.coupling = path.coupling;
    heard.push_back(std::move(row));
  }
  return heard;
}

}  // namespace osats::vhf
