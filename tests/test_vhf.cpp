#include "osats/vhf.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "CHECK failed: " #expr " at " << __FILE__ << ":"         \
                << __LINE__ << "\n";                                         \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

void failPath(const char* expr, const osats::vhf::Path& path, int line) {
  std::cerr << "CHECK failed: " << expr << " at line " << line << "\n"
            << "  audible=" << path.audible << " coupling=" << path.coupling
            << " raw=" << path.raw_coupling
            << " same=" << path.same_frequency
            << " horizon_ok=" << path.within_horizon
            << " terrain_clear=" << path.terrain_clear
            << " dist=" << path.distance_nm << " horizon=" << path.horizon_nm
            << " min_clr=" << path.min_clearance_ft << "\n";
  std::exit(1);
}

#define CHECK_PATH(expr, path)                                               \
  do {                                                                       \
    if (!(expr)) failPath(#expr, path, __LINE__);                            \
  } while (0)

constexpr std::uint32_t kTowerKhz = 118700;
constexpr std::uint32_t kUnicomKhz = 122800;

// East-west ridge. Height falls linearly to the valley at half_width_nm.
class SyntheticRidge final : public osats::vhf::ElevationSampler {
 public:
  double peak_north_nm = 0.0;
  double half_width_nm = 1.0;
  double peak_ft = 0.0;
  double floor_ft = 0.0;

  double elevationMslFt(double latitude_deg, double longitude_deg) const override {
    (void)longitude_deg;
    const double north_nm = latitude_deg * 60.0;
    const double off = std::abs(north_nm - peak_north_nm);
    if (off >= half_width_nm) return floor_ft;
    return floor_ft + peak_ft * (1.0 - off / half_width_nm);
  }
};

// High ground only near one latitude. Elsewhere the valley floor.
class LocalPlateau final : public osats::vhf::ElevationSampler {
 public:
  double center_north_nm = 0.0;
  double radius_nm = 1.0;
  double plateau_ft = 0.0;

  double elevationMslFt(double latitude_deg, double longitude_deg) const override {
    (void)longitude_deg;
    const double north_nm = latitude_deg * 60.0;
    if (std::abs(north_nm - center_north_nm) <= radius_nm) return plateau_ft;
    return 0.0;
  }
};

osats::AircraftState station(osats::EntityId id, osats::Role role, const char* callsign,
                             double north_nm, double alt_ft, std::uint32_t khz,
                             double visibility_nm) {
  osats::AircraftState s;
  s.id = id;
  s.role = role;
  s.callsign = callsign;
  s.radio_hz = khz;
  s.visibility_nm = visibility_nm;
  s.position.latitude_deg = north_nm / 60.0;
  s.position.longitude_deg = 0.0;
  s.position.altitude_ft = alt_ft;
  return s;
}

osats::vhf::Transceiver tuned(const char* callsign, osats::Role role, double north_nm,
                              double alt_ft, std::uint32_t khz) {
  osats::vhf::Transceiver t;
  t.callsign = callsign;
  t.role = role;
  t.latitude_deg = north_nm / 60.0;
  t.longitude_deg = 0.0;
  t.altitude_ft = alt_ft;
  t.frequency_khz = khz;
  return t;
}

bool containsCallsign(const std::vector<osats::vhf::Heard>& rows, const char* callsign) {
  for (const auto& row : rows) {
    if (row.callsign == callsign) return true;
  }
  return false;
}

osats::vhf::Antenna antennaAt(double north_nm, double height_ft, std::uint32_t khz) {
  osats::vhf::Antenna a;
  a.latitude_deg = north_nm / 60.0;
  a.longitude_deg = 0.0;
  a.altitude_msl_ft = height_ft;
  a.height_agl_ft = height_ft;
  a.frequency_khz = khz;
  return a;
}

struct Picture {
  SyntheticRidge ridge;
  osats::vhf::ConstantElevation sea{0.0};
  osats::vhf::FacilityTable facilities;
  osats::AircraftState tower;
  osats::AircraftState ramp;
  osats::AircraftState behind;
  osats::AircraftState high;
  osats::AircraftState far_low;
  osats::AircraftState off_freq;

  Picture()
      : tower(station(1, osats::Role::Controller, "TWR", 600.0, 40000.0, kTowerKhz, 100.0)),
        ramp(station(2, osats::Role::Pilot, "RAMP", -1.0, 0.0, kTowerKhz, 0.5)),
        behind(station(3, osats::Role::Pilot, "BEHIND", 6.0, 0.0, kTowerKhz, 50.0)),
        high(station(4, osats::Role::Pilot, "HIGH", 30.0, 18000.0, kTowerKhz, 0.5)),
        far_low(station(5, osats::Role::Pilot, "FARLOW", 30.0, 0.0, kTowerKhz, 100.0)),
        off_freq(station(6, osats::Role::Pilot, "OFF", -1.0, 0.0, kUnicomKhz, 100.0)) {
    ridge.peak_north_nm = 3.0;
    ridge.half_width_nm = 2.0;
    ridge.peak_ft = 400.0;
    // The blip is not the tower. Longitude is off the path as well.
    tower.position.longitude_deg = 10.0;

    osats::vhf::FacilitySite site;
    site.latitude_deg = 0.0;
    site.longitude_deg = 0.0;
    site.field_elevation_ft = 0.0;
    site.mast_height_ft = 80.0;
    facilities.upsert("TWR", site);
  }
};

void testHorizonFormula() {
  using osats::vhf::radioHorizonNm;
  CHECK(std::abs(radioHorizonNm(100.0, 25.0) - 1.23 * (10.0 + 5.0)) < 1e-12);
  CHECK(std::abs(radioHorizonNm(0.0, 0.0)) < 1e-12);
  CHECK(std::abs(radioHorizonNm(-20.0, 100.0) - radioHorizonNm(0.0, 100.0)) < 1e-12);
  CHECK(std::abs(osats::vhf::kHorizonConstant - 1.23) < 1e-12);
}

void testSurfaceFloorAndLocalGround() {
  CHECK(osats::vhf::kSurfaceFloorFt >= 20.0);
  CHECK(osats::vhf::kSurfaceFloorFt <= 90.0);

  osats::vhf::ConstantElevation mountains(1200.0);
  osats::Position high;
  high.latitude_deg = 1.0;
  high.longitude_deg = 2.0;
  high.altitude_ft = 5000.0;
  const auto up =
      osats::vhf::placeAircraftAntenna(high, kUnicomKhz, mountains);
  CHECK(std::abs(up.height_agl_ft - 3800.0) < 1e-6);
  CHECK(std::abs(up.altitude_msl_ft - 5000.0) < 1e-6);
  CHECK(up.frequency_khz == kUnicomKhz);

  osats::Position pavement;
  pavement.altitude_ft = 1200.0;
  const auto down =
      osats::vhf::placeAircraftAntenna(pavement, kUnicomKhz, mountains);
  CHECK(std::abs(down.height_agl_ft - osats::vhf::kSurfaceFloorFt) < 1e-6);
  CHECK(std::abs(down.altitude_msl_ft - (1200.0 + osats::vhf::kSurfaceFloorFt)) < 1e-6);

  // Reported below the sampled surface: still the floor, not a negative mast.
  osats::Position buried;
  buried.altitude_ft = 1000.0;
  const auto clipped =
      osats::vhf::placeAircraftAntenna(buried, kUnicomKhz, mountains);
  CHECK(std::abs(clipped.height_agl_ft - osats::vhf::kSurfaceFloorFt) < 1e-6);
}

void testFacilityAntennaIgnoresTheBlip() {
  osats::vhf::FacilitySite site;
  site.latitude_deg = 37.62;
  site.longitude_deg = -122.38;
  site.field_elevation_ft = 13.0;
  site.mast_height_ft = 150.0;
  const auto ant = osats::vhf::placeFacilityAntenna(site, kTowerKhz);
  CHECK(std::abs(ant.latitude_deg - 37.62) < 1e-12);
  CHECK(std::abs(ant.longitude_deg - (-122.38)) < 1e-12);
  CHECK(std::abs(ant.height_agl_ft - 150.0) < 1e-12);
  CHECK(std::abs(ant.altitude_msl_ft - 163.0) < 1e-12);
  CHECK(ant.frequency_khz == kTowerKhz);
}

void testPavementFloorOpensAHorizon() {
  osats::vhf::ConstantElevation field(200.0);
  osats::vhf::FacilityTable none;
  auto a = station(1, osats::Role::Pilot, "N1", 0.0, 200.0, kUnicomKhz, 5.0);
  auto b = station(2, osats::Role::Pilot, "N2", 0.5, 200.0, kUnicomKhz, 5.0);
  const auto path = osats::vhf::propagate(a, b, none, field);
  CHECK_PATH(path.audible, path);
  CHECK_PATH(path.coupling > 0.9, path);
  const double horizon =
      1.23 * (std::sqrt(osats::vhf::kSurfaceFloorFt) + std::sqrt(osats::vhf::kSurfaceFloorFt));
  CHECK_PATH(std::abs(path.horizon_nm - horizon) < 1e-6, path);
}

void testControllerSiteNotEntityPosition() {
  osats::vhf::ConstantElevation sea(0.0);
  auto twr = station(1, osats::Role::Controller, "TWR", 0.0, 0.0, kTowerKhz, 50.0);
  auto ramp = station(2, osats::Role::Pilot, "RAMP", 0.3, 0.0, kTowerKhz, 50.0);

  osats::vhf::FacilityTable empty;
  const auto missing = osats::vhf::propagate(twr, ramp, empty, sea);
  CHECK_PATH(missing.same_frequency, missing);
  CHECK_PATH(!missing.audible, missing);

  osats::vhf::FacilityTable far;
  osats::vhf::FacilitySite away;
  away.latitude_deg = 5.0;
  away.longitude_deg = 0.0;
  away.field_elevation_ft = 0.0;
  away.mast_height_ft = 80.0;
  far.upsert("TWR", away);
  const auto from_site = osats::vhf::propagate(twr, ramp, far, sea);
  CHECK_PATH(!from_site.audible, from_site);
  CHECK_PATH(from_site.distance_nm > 200.0, from_site);

  // A facility record does not move a pilot off the reported position.
  osats::vhf::FacilityTable mistaken;
  mistaken.upsert("RAMP", away);
  auto other = station(3, osats::Role::Pilot, "N3", 0.8, 1000.0, kTowerKhz, 10.0);
  const auto pilots = osats::vhf::propagate(ramp, other, mistaken, sea);
  CHECK_PATH(pilots.audible, pilots);
  CHECK_PATH(std::abs(pilots.distance_nm - 0.5) < 0.05, pilots);
}

void testFieldElevationClearsTheAirport() {
  osats::vhf::ConstantElevation field(500.0);
  osats::vhf::FacilityTable facilities;
  osats::vhf::FacilitySite site;
  site.latitude_deg = 0.0;
  site.longitude_deg = 0.0;
  site.field_elevation_ft = 500.0;
  site.mast_height_ft = 80.0;
  facilities.upsert("TWR", site);

  auto twr = station(1, osats::Role::Controller, "TWR", 400.0, 0.0, kTowerKhz, 10.0);
  twr.position.longitude_deg = 8.0;
  auto ramp = station(2, osats::Role::Pilot, "RAMP", 1.0, 500.0, kTowerKhz, 0.2);
  const auto path = osats::vhf::propagate(twr, ramp, facilities, field);
  CHECK_PATH(path.audible, path);
  CHECK_PATH(path.coupling > 0.9, path);
  CHECK_PATH(std::abs(path.distance_nm - 1.0) < 0.05, path);
}

void testWorkedPicture() {
  // Tower at the field. Ramp 1 nm south hears it. An aircraft the same
  // kind of distance north, behind a ridge, does not. A much higher
  // aircraft 30 nm out hears it, and the ridge is under that ray.
  // visibility_nm is set to values that would make the text-radio gate
  // disagree, and it is not the voice range.
  Picture pic;
  const auto ramp = osats::vhf::propagate(pic.tower, pic.ramp, pic.facilities, pic.ridge);
  CHECK_PATH(ramp.audible, ramp);
  CHECK_PATH(ramp.terrain_clear, ramp);
  CHECK_PATH(ramp.coupling > 0.9, ramp);
  CHECK_PATH(std::abs(ramp.coupling - ramp.raw_coupling) < 1e-12, ramp);
  CHECK_PATH(std::abs(ramp.distance_nm - 1.0) < 0.05, ramp);
  const double ramp_horizon =
      1.23 * (std::sqrt(80.0) + std::sqrt(osats::vhf::kSurfaceFloorFt));
  CHECK_PATH(std::abs(ramp.horizon_nm - ramp_horizon) < 1e-6, ramp);

  const auto behind =
      osats::vhf::propagate(pic.tower, pic.behind, pic.facilities, pic.ridge);
  CHECK_PATH(behind.within_horizon, behind);
  CHECK_PATH(!behind.terrain_clear, behind);
  CHECK_PATH(behind.min_clearance_ft < 0.0, behind);
  CHECK_PATH(!behind.audible, behind);
  CHECK_PATH(behind.coupling == 0.0, behind);

  const auto behind_flat =
      osats::vhf::propagate(pic.tower, pic.behind, pic.facilities, pic.sea);
  CHECK_PATH(behind_flat.audible, behind_flat);

  const auto high = osats::vhf::propagate(pic.tower, pic.high, pic.facilities, pic.ridge);
  const auto high_flat =
      osats::vhf::propagate(pic.tower, pic.high, pic.facilities, pic.sea);
  CHECK_PATH(high.audible, high);
  CHECK_PATH(high.terrain_clear, high);
  CHECK_PATH(high.coupling > 0.7, high);
  CHECK_PATH(ramp.coupling > high.coupling, high);
  // Flat earth yields the same coupling, so the ridge is under the ray.
  CHECK_PATH(std::abs(high.coupling - high_flat.coupling) < 1e-9, high);
  const double high_horizon = 1.23 * (std::sqrt(80.0) + std::sqrt(18000.0));
  CHECK_PATH(std::abs(high.horizon_nm - high_horizon) < 1e-4, high);

  // Same place as the high aircraft, on the pavement: the horizon is shut.
  const auto far_low =
      osats::vhf::propagate(pic.tower, pic.far_low, pic.facilities, pic.sea);
  CHECK_PATH(!far_low.within_horizon, far_low);
  CHECK_PATH(!far_low.audible, far_low);

  // Sitting next to the ramp aircraft, on another frequency.
  const auto off =
      osats::vhf::propagate(pic.tower, pic.off_freq, pic.facilities, pic.ridge);
  CHECK_PATH(!off.same_frequency, off);
  CHECK_PATH(!off.audible, off);
  CHECK_PATH(off.coupling == 0.0, off);

  auto adjacent = pic.ramp;
  adjacent.id = 8;
  adjacent.callsign = "ADJ";
  adjacent.radio_hz = kTowerKhz + 25;
  const auto adj = osats::vhf::propagate(pic.tower, adjacent, pic.facilities, pic.sea);
  CHECK_PATH(!adj.same_frequency, adj);
  CHECK_PATH(!adj.audible, adj);

  auto observer = pic.ramp;
  observer.id = 9;
  observer.role = osats::Role::Observer;
  observer.callsign = "OBS";
  const auto obs = osats::vhf::propagate(pic.tower, observer, pic.facilities, pic.ridge);
  CHECK_PATH(obs.audible, obs);

  const auto uplink =
      osats::vhf::propagate(pic.ramp, pic.tower, pic.facilities, pic.ridge);
  CHECK_PATH(uplink.audible, uplink);
  CHECK_PATH(std::abs(uplink.coupling - ramp.coupling) < 1e-6, uplink);
  CHECK_PATH(std::abs(uplink.distance_nm - ramp.distance_nm) < 1e-6, uplink);

  const auto self = osats::vhf::propagate(pic.ramp, pic.ramp, pic.facilities, pic.sea);
  CHECK_PATH(!self.audible, self);
}

void testDifferentFrequencyIsNotAVoicePath() {
  osats::vhf::ConstantElevation sea(0.0);
  osats::vhf::FacilityTable facilities;
  osats::vhf::FacilitySite site;
  site.mast_height_ft = 80.0;
  facilities.upsert("TWR", site);

  auto twr = station(1, osats::Role::Controller, "TWR", 0.0, 0.0, kTowerKhz, 50.0);
  auto on_freq = station(2, osats::Role::Pilot, "ON", 0.2, 0.0, kTowerKhz, 50.0);
  auto other = station(3, osats::Role::Pilot, "OFF", 0.2, 0.0, kUnicomKhz, 50.0);

  const auto heard = osats::vhf::propagate(twr, on_freq, facilities, sea);
  const auto missed = osats::vhf::propagate(twr, other, facilities, sea);
  CHECK_PATH(heard.audible, heard);
  CHECK_PATH(heard.coupling > 0.9, heard);
  CHECK_PATH(!missed.same_frequency, missed);
  CHECK_PATH(!missed.audible, missed);
  CHECK_PATH(missed.coupling == 0.0, missed);
}

void testHorizonUsesHeightAboveLocalGround() {
  auto twr = station(1, osats::Role::Controller, "TWR", 900.0, 20000.0, kTowerKhz, 1.0);
  twr.position.longitude_deg = -20.0;
  auto craft = station(2, osats::Role::Pilot, "N4", 40.0, 10000.0, kTowerKhz, 1.0);

  osats::vhf::FacilityTable facilities;
  osats::vhf::FacilitySite site;
  site.mast_height_ft = 80.0;
  facilities.upsert("TWR", site);

  osats::vhf::ConstantElevation sea(0.0);
  const auto over_sea = osats::vhf::propagate(twr, craft, facilities, sea);
  CHECK_PATH(over_sea.audible, over_sea);
  CHECK_PATH(over_sea.within_horizon, over_sea);

  LocalPlateau plateau;
  plateau.center_north_nm = 40.0;
  plateau.radius_nm = 2.0;
  plateau.plateau_ft = 10000.0 - osats::vhf::kSurfaceFloorFt;
  const auto on_plateau = osats::vhf::propagate(twr, craft, facilities, plateau);
  CHECK_PATH(!on_plateau.within_horizon, on_plateau);
  CHECK_PATH(!on_plateau.audible, on_plateau);
  const double short_horizon =
      1.23 * (std::sqrt(80.0) + std::sqrt(osats::vhf::kSurfaceFloorFt));
  CHECK_PATH(std::abs(on_plateau.horizon_nm - short_horizon) < 1e-6, on_plateau);
  CHECK(on_plateau.distance_nm > on_plateau.horizon_nm);
}

void testWeakerTowardTheHorizonAndSquelch() {
  CHECK(osats::vhf::kSquelch > 0.0);
  CHECK(osats::vhf::kSquelch < 0.5);

  osats::vhf::ConstantElevation sea(0.0);
  const double h = 10000.0;
  const double horizon = osats::vhf::radioHorizonNm(h, h);
  auto at = [&](double fraction) {
    return osats::vhf::evaluate(antennaAt(0.0, h, kUnicomKhz),
                                antennaAt(fraction * horizon, h, kUnicomKhz), sea);
  };
  const auto near = at(0.05);
  const auto mid = at(0.30);
  const auto far = at(0.60);
  CHECK_PATH(near.audible && mid.audible && far.audible, far);
  CHECK_PATH(near.coupling > 0.9, near);
  CHECK_PATH(near.coupling > mid.coupling, mid);
  CHECK_PATH(mid.coupling > far.coupling, far);
  CHECK_PATH(far.coupling < 0.5, far);

  const auto edge = osats::vhf::evaluate(antennaAt(0.0, 100.0, kTowerKhz),
                                         antennaAt(0.95 * osats::vhf::radioHorizonNm(100.0, 100.0),
                                                   100.0, kTowerKhz),
                                         sea);
  CHECK_PATH(edge.within_horizon, edge);
  CHECK_PATH(edge.terrain_clear, edge);
  CHECK_PATH(edge.min_clearance_ft > 2.0, edge);
  CHECK_PATH(edge.min_clearance_ft < 30.0, edge);
  CHECK_PATH(edge.raw_coupling > 0.0, edge);
  CHECK_PATH(edge.raw_coupling < osats::vhf::kSquelch, edge);
  CHECK_PATH(!edge.audible, edge);
  CHECK_PATH(edge.coupling == 0.0, edge);
}

void testHearSetWorkedPicture() {
  // Same geometry as the pair tests: ramp hears, the ridge blocks, the high
  // aircraft hears, and the other frequency is absent. Candidate order is kept.
  SyntheticRidge ridge;
  ridge.peak_north_nm = 3.0;
  ridge.half_width_nm = 2.0;
  ridge.peak_ft = 400.0;

  osats::vhf::FacilitySite site;
  site.mast_height_ft = 80.0;

  auto tower = tuned("TWR", osats::Role::Controller, 600.0, 40000.0, kTowerKhz);
  tower.longitude_deg = 10.0;
  tower.facility = &site;

  const auto behind = tuned("BEHIND", osats::Role::Pilot, 6.0, 0.0, kTowerKhz);
  const auto off = tuned("OFF", osats::Role::Pilot, -1.0, 0.0, kUnicomKhz);
  const auto high = tuned("HIGH", osats::Role::Pilot, 30.0, 18000.0, kTowerKhz);
  const auto ramp = tuned("RAMP", osats::Role::Pilot, -1.0, 0.0, kTowerKhz);
  const osats::vhf::Transceiver candidates[] = {behind, off, high, ramp, tower};
  const auto heard = osats::vhf::hearSet(tower, candidates, ridge);

  CHECK(heard.size() == 2);
  CHECK(heard[0].callsign == "HIGH");
  CHECK(heard[1].callsign == "RAMP");
  CHECK(heard[0].coupling > 0.7);
  CHECK(heard[1].coupling > 0.9);
  CHECK(heard[1].coupling > heard[0].coupling);
  CHECK(!containsCallsign(heard, "BEHIND"));
  CHECK(!containsCallsign(heard, "OFF"));
  CHECK(!containsCallsign(heard, "TWR"));

  osats::vhf::FacilityTable facilities;
  facilities.upsert("TWR", site);
  osats::AircraftState tx;
  tx.id = 1;
  tx.callsign = "TWR";
  tx.role = osats::Role::Controller;
  tx.radio_hz = kTowerKhz;
  tx.position.latitude_deg = tower.latitude_deg;
  tx.position.longitude_deg = tower.longitude_deg;
  tx.position.altitude_ft = tower.altitude_ft;
  auto as_state = [](const osats::vhf::Transceiver& t, osats::EntityId id) {
    osats::AircraftState state;
    state.id = id;
    state.callsign = t.callsign;
    state.role = t.role;
    state.radio_hz = t.frequency_khz;
    state.position.latitude_deg = t.latitude_deg;
    state.position.longitude_deg = t.longitude_deg;
    state.position.altitude_ft = t.altitude_ft;
    return state;
  };
  const auto high_path = osats::vhf::propagate(tx, as_state(high, 4), facilities, ridge);
  const auto ramp_path = osats::vhf::propagate(tx, as_state(ramp, 2), facilities, ridge);
  CHECK(high_path.audible && ramp_path.audible);
  CHECK(std::abs(heard[0].coupling - high_path.coupling) < 1e-12);
  CHECK(std::abs(heard[1].coupling - ramp_path.coupling) < 1e-12);
}

void testTwoTransmittersOnlyMatchingFrequency() {
  // Both transmitters are close to both receivers. Each hear-set keeps only
  // the receiver on that transmission's frequency.
  osats::vhf::ConstantElevation sea(0.0);
  const auto on_tower = tuned("T118", osats::Role::Pilot, 0.0, 500.0, kTowerKhz);
  const auto on_unicom = tuned("T122", osats::Role::Pilot, 0.15, 500.0, kUnicomKhz);
  const auto hear_tower = tuned("R118", osats::Role::Pilot, 0.4, 500.0, kTowerKhz);
  const auto hear_unicom = tuned("R122", osats::Role::Pilot, 0.5, 500.0, kUnicomKhz);
  const osats::vhf::Transceiver candidates[] = {hear_tower, hear_unicom, on_tower, on_unicom};

  const auto tower_set = osats::vhf::hearSet(on_tower, candidates, sea);
  CHECK(tower_set.size() == 1);
  CHECK(tower_set[0].callsign == "R118");
  CHECK(tower_set[0].coupling > 0.9);
  CHECK(!containsCallsign(tower_set, "R122"));
  CHECK(!containsCallsign(tower_set, "T122"));
  CHECK(!containsCallsign(tower_set, "T118"));

  const auto unicom_set = osats::vhf::hearSet(on_unicom, candidates, sea);
  CHECK(unicom_set.size() == 1);
  CHECK(unicom_set[0].callsign == "R122");
  CHECK(unicom_set[0].coupling > 0.9);
  CHECK(!containsCallsign(unicom_set, "R118"));
  CHECK(!containsCallsign(unicom_set, "T118"));
  CHECK(!containsCallsign(unicom_set, "T122"));
}

void testGrazingRidgeIsWeaker() {
  osats::vhf::ConstantElevation sea(0.0);
  SyntheticRidge ridge;
  ridge.peak_north_nm = 2.5;
  ridge.half_width_nm = 1.5;
  ridge.peak_ft = 900.0;

  const auto tx = antennaAt(0.0, 80.0, kTowerKhz);
  osats::vhf::Antenna rx = antennaAt(5.0, 2000.0, kTowerKhz);
  const auto clear = osats::vhf::evaluate(tx, rx, sea);
  const auto graze = osats::vhf::evaluate(tx, rx, ridge);
  CHECK_PATH(clear.audible, clear);
  CHECK_PATH(graze.audible, graze);
  CHECK_PATH(graze.terrain_clear, graze);
  CHECK_PATH(graze.min_clearance_ft > 0.0, graze);
  CHECK_PATH(graze.coupling < clear.coupling * 0.75, graze);
}

}  // namespace

int main() {
  testHorizonFormula();
  testSurfaceFloorAndLocalGround();
  testFacilityAntennaIgnoresTheBlip();
  testPavementFloorOpensAHorizon();
  testControllerSiteNotEntityPosition();
  testFieldElevationClearsTheAirport();
  testWorkedPicture();
  testDifferentFrequencyIsNotAVoicePath();
  testHorizonUsesHeightAboveLocalGround();
  testWeakerTowardTheHorizonAndSquelch();
  testHearSetWorkedPicture();
  testTwoTransmittersOnlyMatchingFrequency();
  testGrazingRidgeIsWeaker();
  std::cout << "test_vhf ok\n";
  return 0;
}
