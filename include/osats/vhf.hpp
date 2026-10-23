#pragma once

#include "osats/types.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace osats::vhf {

// 4/3-earth radio horizon: nautical miles from antenna heights in feet.
// d = 1.23 * (sqrt(h_tx) + sqrt(h_rx)). Tunable without changing the model.
inline constexpr double kHorizonConstant = 1.23;

// Aircraft on the pavement still have a short antenna. Tens of feet.
inline constexpr double kSurfaceFloorFt = 50.0;

// Coupling below this is silence. An open path reports coupling in [squelch, 1].
inline constexpr double kSquelch = 0.10;

// Fraction of the ray's height over the ground chord that counts as well
// clear. Thinner clearance scales coupling down; a hit blocks the path.
inline constexpr double kWellClearFraction = 0.25;

// Ground-track spacing for occlusion samples.
inline constexpr double kSampleSpacingNm = 0.1;

struct Params {
  double horizon_constant = kHorizonConstant;
  double surface_floor_ft = kSurfaceFloorFt;
  double squelch = kSquelch;
  double well_clear_fraction = kWellClearFraction;
  double sample_spacing_nm = kSampleSpacingNm;
};

// One published transmitter site for a control position.
struct FacilitySite {
  double latitude_deg = 0.0;
  double longitude_deg = 0.0;
  double field_elevation_ft = 0.0;  // MSL
  double mast_height_ft = 0.0;      // above the field
};

// Sites keyed by controller callsign. Pilots and observers are not listed.
class FacilityTable {
 public:
  void upsert(const std::string& callsign, FacilitySite site) {
    sites_[callsign] = site;
  }

  const FacilitySite* find(const std::string& callsign) const {
    const auto it = sites_.find(callsign);
    return it == sites_.end() ? nullptr : &it->second;
  }

 private:
  std::unordered_map<std::string, FacilitySite> sites_;
};

// Terrain elevation MSL, feet, at a point. The host supplies the dataset.
class ElevationSampler {
 public:
  virtual ~ElevationSampler() = default;
  virtual double elevationMslFt(double latitude_deg, double longitude_deg) const = 0;
};

class ConstantElevation final : public ElevationSampler {
 public:
  explicit ConstantElevation(double elevation_msl_ft = 0.0)
      : elevation_msl_ft_(elevation_msl_ft) {}

  double elevationMslFt(double, double) const override { return elevation_msl_ft_; }

 private:
  double elevation_msl_ft_;
};

// A placed transceiver. Height is above the local ground; altitude is MSL.
struct Antenna {
  double latitude_deg = 0.0;
  double longitude_deg = 0.0;
  double altitude_msl_ft = 0.0;
  double height_agl_ft = 0.0;
  std::uint32_t frequency_khz = 0;  // same integer as AircraftState::radio_hz
};

// Result of one transmitter/receiver pair. The sender is not a receiver.
// coupling is 0 when the receiver stays silent, including below squelch.
// raw_coupling is the quality before that gate, still 0 when the path is
// the wrong frequency, over the horizon, or blocked by terrain.
struct Path {
  bool same_frequency = false;
  bool within_horizon = false;
  bool terrain_clear = false;
  bool audible = false;
  double coupling = 0.0;
  double raw_coupling = 0.0;
  double distance_nm = 0.0;
  double horizon_nm = 0.0;
  double min_clearance_ft = 0.0;
};

inline double radioHorizonNm(double height_tx_ft, double height_rx_ft,
                             double horizon_constant = kHorizonConstant) {
  if (!(horizon_constant > 0.0)) return 0.0;
  const double h_tx = height_tx_ft > 0.0 ? height_tx_ft : 0.0;
  const double h_rx = height_rx_ft > 0.0 ? height_rx_ft : 0.0;
  return horizon_constant * (std::sqrt(h_tx) + std::sqrt(h_rx));
}

Antenna placeAircraftAntenna(const Position& position, std::uint32_t frequency_khz,
                             const ElevationSampler& terrain,
                             const Params& params = {});

// Mast and field elevation. The controller entity position is not an input.
Antenna placeFacilityAntenna(const FacilitySite& site, std::uint32_t frequency_khz);

Path evaluate(const Antenna& tx, const Antenna& rx, const ElevationSampler& terrain,
              const Params& params = {});

// Controller antennas come from the facility table. Aircraft and observers
// use the reported position. A controller with no site has no voice path.
Path propagate(const AircraftState& transmitter, const AircraftState& receiver,
               const FacilityTable& facilities, const ElevationSampler& terrain,
               const Params& params = {});

}  // namespace osats::vhf
