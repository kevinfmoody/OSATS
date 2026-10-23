#include "osats/client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// Injected KSFO pilots. Arrivals stay open-loop on UNICOM (122.800).
// Aircraft in the pattern tune the tower (120.500), read back a small set of
// instructions, and copy the result into the positions they publish.
// Anything else gets one "say again"; the current path stays.
// The sender is whoever transmitted: the automated tower, a pilot, or a human
// who has taken KSFO_TWR. Nothing here keys off the bot's name.
//
// Usage: osats-traffic <host> <port> [seconds]
// Omit seconds to run until SIGINT/SIGTERM.

namespace {

constexpr double kFieldLat = 37.618806;
constexpr double kFieldLon = -122.375417;
constexpr std::uint32_t kUnicomHz = 122800;
constexpr std::uint32_t kTowerHz = 120500;
constexpr double kRunwayHdg = 280.0;
constexpr double kFirstCallDelayS = 0.5;
constexpr double kVisibilityNm = 40.0;
constexpr const char* kOperatorName = "OSATS Traffic";
constexpr auto kTick = std::chrono::milliseconds(200);

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

enum class Maneuver { Script, HoldShort, LineUp, Takeoff, Land, Fly };

struct Fix {
  double north_nm = 0;
  double east_nm = 0;
  double altitude_ft = 0;
  double groundspeed_kt = 0;
  const char* call = nullptr;
};

struct Instruction {
  bool recognized = false;
  bool hold_short = false;
  bool line_up = false;
  bool takeoff = false;
  bool land = false;
  bool runway_heading = false;
  std::optional<double> heading_deg;
  std::optional<double> altitude_ft;
  std::optional<std::uint16_t> squawk;
  std::string squawk_text;
  std::optional<std::uint32_t> contact_hz;
  std::string contact_text;
  osats::TextScope scope = osats::TextScope::Radio;
  std::string from;
  std::string raw;
};

struct Inbox {
  std::mutex mu;
  std::vector<Instruction> items;
  // One "say again" per distinct text that is not in the action set.
  std::unordered_set<std::string> said_again;
};

struct Aircraft {
  std::string callsign;
  std::uint16_t squawk = 1200;
  std::uint32_t radio_hz = kUnicomHz;
  bool pattern = false;
  osats::FlightPlan plan;
  std::vector<Fix> path;
  std::size_t leg = 0;
  double leg_elapsed_s = 0;
  double leg_duration_s = 1;
  double airborne_s = 0;
  const char* pending_call = nullptr;

  Maneuver maneuver = Maneuver::Script;
  bool kin_live = false;
  double north_nm = 0;
  double east_nm = 0;
  double alt_ft = 0;
  double gs_kt = 0;
  double hdg_deg = 0;
  double vs_fps = 0;
  bool have_heading = false;
  double target_heading = kRunwayHdg;
  bool have_alt = false;
  double target_alt_ft = 3000;

  std::shared_ptr<Inbox> inbox;
  std::unique_ptr<osats::Client> client;
};

osats::FlightPlan makePlan(char rules, const char* type, const char* dep, const char* arr,
                           std::uint16_t cruise_ft, std::uint16_t tas_kt, const char* route) {
  osats::FlightPlan fp;
  fp.flight_rules = rules;
  fp.aircraft_type = type;
  fp.departure = dep;
  fp.arrival = arr;
  fp.cruise_alt_ft = cruise_ft;
  fp.tas_kt = tas_kt;
  fp.route = route;
  fp.remarks = "OSATS traffic";
  return fp;
}

std::string lowerCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string fmtFreq(std::uint32_t hz) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.3f", hz / 1000.0);
  return buf;
}

double lonScale() { return std::cos(kFieldLat * std::numbers::pi / 180.0); }

std::vector<Aircraft> scriptedFleet() {
  std::vector<Aircraft> fleet;

  {
    Aircraft ac;
    ac.callsign = "AAL142";
    ac.squawk = 4201;
    ac.plan = makePlan('I', "A321", "KLAX", "KSFO", 34000, 440, "SERFR KSFO");
    ac.path = {
        {-6.0, 14.0, 6000, 220,
         "KSFO traffic, American 142, 15 miles southeast, inbound runway 28, American 142"},
        {-3.0, 9.0, 3000, 180, nullptr},
        {-1.2, 4.5, 1400, 150,
         "KSFO traffic, American 142, short final runway 28, American 142"},
        {0.4, 0.2, 500, 140, nullptr},
        {2.0, -5.0, 2500, 180, nullptr},
        {8.0, -6.0, 5000, 220, nullptr},
        {8.0, 8.0, 6000, 230, nullptr},
        {-2.0, 16.0, 6000, 220, nullptr},
    };
    fleet.push_back(std::move(ac));
  }
  {
    Aircraft ac;
    ac.callsign = "UAL482";
    ac.squawk = 4202;
    ac.plan = makePlan('I', "B739", "KSEA", "KSFO", 35000, 450, "PYE BDEGA KSFO");
    ac.path = {
        {15.0, 4.0, 7000, 210,
         "KSFO traffic, United 482, 15 miles north, inbound for landing, United 482"},
        {8.0, 5.0, 4000, 180, nullptr},
        {2.0, 6.0, 2200, 160, nullptr},
        {-4.0, 7.0, 1800, 150,
         "KSFO traffic, United 482, east of the field, southbound, United 482"},
        {-8.0, 2.0, 3500, 180, nullptr},
        {-4.0, -6.0, 5500, 210, nullptr},
        {6.0, -4.0, 7000, 220, nullptr},
        {15.0, 0.0, 7000, 210, nullptr},
    };
    fleet.push_back(std::move(ac));
  }
  {
    // Departure, in the pattern. Slow taxi until a clearance stops or launches it.
    Aircraft ac;
    ac.callsign = "SWA2104";
    ac.squawk = 4203;
    ac.pattern = true;
    ac.radio_hz = kTowerHz;
    ac.plan = makePlan('I', "B738", "KSFO", "KLAX", 36000, 450, "SSTIK3 PORTE");
    ac.path = {
        {0.10, 0.40, 20, 20, nullptr}, {1.5, -4.0, 2500, 190, nullptr},
        {4.0, -10.0, 7000, 250, nullptr}, {10.0, -12.0, 12000, 280, nullptr},
        {14.0, -4.0, 15000, 300, nullptr}, {12.0, 8.0, 16000, 300, nullptr},
        {4.0, 10.0, 8000, 250, nullptr},  {0.5, 3.0, 2000, 180, nullptr},
    };
    fleet.push_back(std::move(ac));
  }
  {
    Aircraft ac;
    ac.callsign = "DAL415";
    ac.squawk = 4204;
    ac.pattern = true;
    ac.radio_hz = kTowerHz;
    ac.plan = makePlan('I', "B739", "KSFO", "KATL", 38000, 460, "SSTIK3 WAGES");
    ac.path = {
        {0.0, -0.04, 20, 20, nullptr}, {1.0, -6.0, 4000, 210, nullptr},
        {-4.0, -10.0, 10000, 270, nullptr}, {-12.0, -8.0, 18000, 300, nullptr},
        {-12.0, 0.0, 20000, 300, nullptr},   {-6.0, 8.0, 14000, 280, nullptr},
        {0.0, 6.0, 6000, 220, nullptr},      {0.2, 1.5, 1500, 170, nullptr},
    };
    fleet.push_back(std::move(ac));
  }
  {
    Aircraft ac;
    ac.callsign = "N172SP";
    ac.squawk = 1200;
    ac.pattern = true;
    ac.radio_hz = kTowerHz;
    ac.plan = makePlan('V', "C172", "KSFO", "KOAK", 2500, 110, "VFR BAY");
    ac.path = {
        {2.0, 4.0, 1500, 90, nullptr},
        {6.0, 5.0, 1500, 90, nullptr},
        {6.0, 9.0, 1500, 90, nullptr},
        {2.0, 8.0, 1500, 90, nullptr},
    };
    fleet.push_back(std::move(ac));
  }
  return fleet;
}

void armLeg(Aircraft& ac) {
  const Fix& from = ac.path[ac.leg];
  const Fix& to = ac.path[(ac.leg + 1) % ac.path.size()];
  const double dist = std::hypot(to.north_nm - from.north_nm, to.east_nm - from.east_nm);
  const double gs = std::max(from.groundspeed_kt, 30.0);
  ac.leg_duration_s = std::max(dist / gs * 3600.0, 0.5);
  ac.pending_call = from.call;
}

osats::Position sampleScript(const Aircraft& ac) {
  const Fix& from = ac.path[ac.leg];
  const Fix& to = ac.path[(ac.leg + 1) % ac.path.size()];
  const double u = std::clamp(ac.leg_elapsed_s / ac.leg_duration_s, 0.0, 1.0);
  const double north = from.north_nm + (to.north_nm - from.north_nm) * u;
  const double east = from.east_nm + (to.east_nm - from.east_nm) * u;
  const double dn = to.north_nm - from.north_nm;
  const double de = to.east_nm - from.east_nm;
  const double hdg_rad = std::atan2(de, dn);
  double hdg_deg = hdg_rad * (180.0 / std::numbers::pi);
  if (hdg_deg < 0) hdg_deg += 360.0;

  const double climb_fps = (to.altitude_ft - from.altitude_ft) / ac.leg_duration_s;
  const double gs_fps = from.groundspeed_kt * 1.68780986;

  osats::Position pos;
  pos.latitude_deg = kFieldLat + north / 60.0;
  pos.longitude_deg = kFieldLon + east / (60.0 * lonScale());
  pos.altitude_ft = from.altitude_ft + (to.altitude_ft - from.altitude_ft) * u;
  pos.groundspeed_kt = from.groundspeed_kt;
  pos.heading_deg = hdg_deg;
  pos.pitch_deg = std::atan2(climb_fps, std::max(gs_fps, 1.0)) * (180.0 / std::numbers::pi);
  pos.vn_fps = static_cast<float>(gs_fps * std::cos(hdg_rad));
  pos.ve_fps = static_cast<float>(gs_fps * std::sin(hdg_rad));
  pos.vd_fps = static_cast<float>(-climb_fps);
  pos.squawk = ac.squawk;
  pos.xpdr_mode = 'C';
  return pos;
}

void advanceScript(Aircraft& ac, double dt) {
  ac.leg_elapsed_s += dt;
  while (ac.leg_elapsed_s >= ac.leg_duration_s) {
    ac.leg_elapsed_s -= ac.leg_duration_s;
    ac.leg = (ac.leg + 1) % ac.path.size();
    armLeg(ac);
  }
}

void captureKin(Aircraft& ac) {
  if (ac.kin_live) return;
  const osats::Position pos = sampleScript(ac);
  ac.north_nm = (pos.latitude_deg - kFieldLat) * 60.0;
  ac.east_nm = (pos.longitude_deg - kFieldLon) * 60.0 * lonScale();
  ac.alt_ft = pos.altitude_ft;
  ac.gs_kt = pos.groundspeed_kt;
  ac.hdg_deg = pos.heading_deg;
  ac.kin_live = true;
}

double wrapHdg(double deg) {
  while (deg < 0) deg += 360.0;
  while (deg >= 360.0) deg -= 360.0;
  return deg;
}

double stepToward(double current, double target, double max_step) {
  double diff = target - current;
  while (diff > 180.0) diff -= 360.0;
  while (diff < -180.0) diff += 360.0;
  if (std::abs(diff) <= max_step) return wrapHdg(target);
  return wrapHdg(current + std::copysign(max_step, diff));
}

double stepValue(double current, double target, double max_step) {
  const double diff = target - current;
  if (std::abs(diff) <= max_step) return target;
  return current + std::copysign(max_step, diff);
}

void move(Aircraft& ac, double dt, double hdg, double gs) {
  const double rad = hdg * std::numbers::pi / 180.0;
  const double nm = gs / 3600.0 * dt;
  ac.north_nm += std::cos(rad) * nm;
  ac.east_nm += std::sin(rad) * nm;
  ac.hdg_deg = hdg;
  ac.gs_kt = gs;
}

osats::Position fromKin(const Aircraft& ac) {
  const double hdg_rad = ac.hdg_deg * std::numbers::pi / 180.0;
  const double gs_fps = ac.gs_kt * 1.68780986;
  osats::Position pos;
  pos.latitude_deg = kFieldLat + ac.north_nm / 60.0;
  pos.longitude_deg = kFieldLon + ac.east_nm / (60.0 * lonScale());
  pos.altitude_ft = ac.alt_ft;
  pos.groundspeed_kt = ac.gs_kt;
  pos.heading_deg = ac.hdg_deg;
  pos.pitch_deg = std::atan2(ac.vs_fps, std::max(gs_fps, 1.0)) * (180.0 / std::numbers::pi);
  pos.vn_fps = static_cast<float>(gs_fps * std::cos(hdg_rad));
  pos.ve_fps = static_cast<float>(gs_fps * std::sin(hdg_rad));
  pos.vd_fps = static_cast<float>(-ac.vs_fps);
  pos.squawk = ac.squawk;
  pos.xpdr_mode = 'C';
  return pos;
}

void stepManeuver(Aircraft& ac, double dt) {
  const double climb = 2400.0 / 60.0 * dt;
  ac.vs_fps = 0;
  switch (ac.maneuver) {
    case Maneuver::Script:
      return;
    case Maneuver::HoldShort:
      ac.gs_kt = 0;
      if (ac.have_heading) ac.hdg_deg = ac.target_heading;
      return;
    case Maneuver::LineUp: {
      const double dist = std::hypot(ac.north_nm, ac.east_nm);
      if (dist < 0.01) {
        ac.gs_kt = 0;
        ac.hdg_deg = ac.have_heading ? ac.target_heading : kRunwayHdg;
        return;
      }
      // Stay under 40 kt so the tower's hold-short classification does not
      // flip while the aircraft taxis onto the runway, then flip back and
      // stop it again. 0.04 nm at 30 kt is a few seconds.
      const double hdg = wrapHdg(std::atan2(-ac.east_nm, -ac.north_nm) * (180.0 / std::numbers::pi));
      move(ac, dt, hdg, 30.0);
      return;
    }
    case Maneuver::Takeoff: {
      const double gs = std::min(170.0, ac.gs_kt + 50.0 * dt);
      const double hdg = ac.have_heading ? ac.target_heading : kRunwayHdg;
      const double target_alt = ac.have_alt ? ac.target_alt_ft : 3000.0;
      const double next_alt = stepValue(ac.alt_ft, target_alt, climb);
      ac.vs_fps = (next_alt - ac.alt_ft) / dt;
      ac.alt_ft = next_alt;
      move(ac, dt, hdg, gs);
      return;
    }
    case Maneuver::Land: {
      const double dist = std::hypot(ac.north_nm, ac.east_nm);
      const double glide = std::max(13.0, dist * 250.0);
      const double next_alt = stepValue(ac.alt_ft, glide, climb);
      ac.vs_fps = (next_alt - ac.alt_ft) / dt;
      ac.alt_ft = next_alt;
      if (dist < 0.02) {
        ac.gs_kt = 0;
        ac.alt_ft = 13;
        ac.hdg_deg = kRunwayHdg;
        return;
      }
      const double hdg = wrapHdg(std::atan2(-ac.east_nm, -ac.north_nm) * (180.0 / std::numbers::pi));
      move(ac, dt, hdg, 110.0);
      return;
    }
    case Maneuver::Fly: {
      const double hdg =
          ac.have_heading ? stepToward(ac.hdg_deg, ac.target_heading, 90.0 * dt) : ac.hdg_deg;
      if (ac.have_alt) {
        const double next_alt = stepValue(ac.alt_ft, ac.target_alt_ft, climb);
        ac.vs_fps = (next_alt - ac.alt_ft) / dt;
        ac.alt_ft = next_alt;
      }
      const double gs = std::max(ac.gs_kt, 90.0);
      move(ac, dt, hdg, gs);
      return;
    }
  }
}

std::optional<double> numberAfter(const std::string& s, std::size_t from) {
  std::size_t i = from;
  const std::size_t limit = std::min(s.size(), from + 28);
  while (i < limit && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i >= limit) return std::nullopt;
  if (i > 0 && s[i - 1] == '.') return std::nullopt;
  std::size_t j = i;
  while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
  if (j < s.size() && s[j] == '.') return std::nullopt;
  try {
    return std::stod(s.substr(i, j - i));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::pair<std::uint32_t, std::string>> findFrequency(const std::string& raw) {
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(raw[i]))) continue;
    std::size_t j = i;
    while (j < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j]))) ++j;
    if (j >= raw.size() || raw[j] != '.') continue;
    std::size_t k = j + 1;
    while (k < raw.size() && std::isdigit(static_cast<unsigned char>(raw[k]))) ++k;
    const std::size_t frac = k - (j + 1);
    if (j - i != 3 || frac < 1 || frac > 3) {
      i = j;
      continue;
    }
    const std::string text = raw.substr(i, k - i);
    double mhz = 0;
    try {
      mhz = std::stod(text);
    } catch (const std::exception&) {
      continue;
    }
    if (mhz < 118.0 || mhz > 137.0) continue;
    return std::make_pair(static_cast<std::uint32_t>(std::llround(mhz * 1000.0)), text);
  }
  return std::nullopt;
}

bool forThisAircraft(const Aircraft& ac, const osats::TextEvent& ev) {
  if (ev.from == ac.callsign) return false;
  if (ev.scope == osats::TextScope::Private) return ev.to == ac.callsign;
  if (ev.scope == osats::TextScope::Radio) {
    return lowerCopy(ev.text).find(lowerCopy(ac.callsign)) != std::string::npos;
  }
  return false;
}

Instruction parseInstruction(const Aircraft& ac, const osats::TextEvent& ev) {
  Instruction ins;
  ins.scope = ev.scope;
  ins.from = ev.from;
  ins.raw = ev.text;
  const std::string low = lowerCopy(ev.text);
  (void)ac;

  ins.hold_short = low.find("hold short") != std::string::npos;
  ins.line_up = low.find("line up") != std::string::npos;
  ins.takeoff = low.find("cleared for takeoff") != std::string::npos ||
                low.find("cleared takeoff") != std::string::npos;
  ins.land = low.find("cleared to land") != std::string::npos;
  ins.runway_heading = low.find("runway heading") != std::string::npos;
  if (!ins.runway_heading) {
    const auto heading = low.find("heading");
    if (heading != std::string::npos) {
      if (auto n = numberAfter(low, heading + 7)) {
        if (*n >= 0 && *n < 360) ins.heading_deg = *n;
      }
    }
  }
  for (const char* key : {"climb", "descend", "maintain"}) {
    const auto at = low.find(key);
    if (at == std::string::npos) continue;
    if (auto n = numberAfter(low, at + std::char_traits<char>::length(key))) {
      if (*n >= 100.0 && *n <= 60000.0) ins.altitude_ft = *n;
    }
  }
  const auto squawk = low.find("squawk");
  if (squawk != std::string::npos) {
    std::size_t i = squawk + 6;
    while (i < low.size() && !std::isdigit(static_cast<unsigned char>(low[i]))) ++i;
    std::size_t j = i;
    while (j < low.size() && std::isdigit(static_cast<unsigned char>(low[j]))) ++j;
    if (j - i == 4) {
      ins.squawk_text = low.substr(i, 4);
      ins.squawk = static_cast<std::uint16_t>(std::stoi(ins.squawk_text));
    }
  }
  if (low.find("contact") != std::string::npos) {
    if (auto freq = findFrequency(ev.text)) {
      ins.contact_hz = freq->first;
      ins.contact_text = freq->second;
    }
  }
  ins.recognized = ins.hold_short || ins.line_up || ins.takeoff || ins.land ||
                  ins.runway_heading || ins.heading_deg || ins.altitude_ft || ins.squawk ||
                  ins.contact_hz;
  return ins;
}

std::string readbackText(const Aircraft& ac, const Instruction& ins) {
  if (!ins.recognized) return ac.callsign + ", say again";
  std::vector<std::string> parts;
  if (ins.hold_short) parts.emplace_back("holding short runway 28");
  if (ins.line_up) parts.emplace_back("lining up runway 28");
  if (ins.takeoff) parts.emplace_back("cleared for takeoff runway 28");
  if (ins.land) parts.emplace_back("cleared to land runway 28");
  if (ins.runway_heading) parts.emplace_back("runway heading");
  else if (ins.heading_deg) {
    parts.push_back("heading " + std::to_string(static_cast<int>(std::lround(*ins.heading_deg))));
  }
  if (ins.altitude_ft) {
    const char* verb = *ins.altitude_ft >= ac.alt_ft ? "climbing to " : "descending to ";
    parts.push_back(verb + std::to_string(static_cast<int>(std::lround(*ins.altitude_ft))));
  }
  if (ins.squawk) parts.push_back("squawk " + ins.squawk_text);
  if (ins.contact_hz) parts.push_back("contacting " + ins.contact_text);
  std::string text = ac.callsign;
  for (const auto& part : parts) text += ", " + part;
  return text;
}

void applyInstruction(Aircraft& ac, const Instruction& ins) {
  if (!ins.recognized) {
    if (!ac.inbox->said_again.insert(ins.raw).second) return;
  } else {
    if (ins.hold_short) ac.maneuver = Maneuver::HoldShort;
    if (ins.line_up) ac.maneuver = Maneuver::LineUp;
    if (ins.land) ac.maneuver = Maneuver::Land;
    if (ins.takeoff) ac.maneuver = Maneuver::Takeoff;
    if (ins.heading_deg) {
      ac.have_heading = true;
      ac.target_heading = *ins.heading_deg;
    }
    if (ins.runway_heading) {
      ac.have_heading = true;
      ac.target_heading = kRunwayHdg;
    }
    if (ins.altitude_ft) {
      ac.have_alt = true;
      ac.target_alt_ft = *ins.altitude_ft;
      if (ac.maneuver == Maneuver::Script || ac.maneuver == Maneuver::Land) {
        ac.maneuver = Maneuver::Fly;
      }
    }
    if ((ins.heading_deg || ins.runway_heading) && ac.maneuver == Maneuver::Script) {
      ac.maneuver = Maneuver::Fly;
    }
    if (ac.maneuver != Maneuver::Script) captureKin(ac);
    // A heading shows up on the next position even while holding short.
    // Line up keeps the taxi heading until the aircraft is on the runway.
    if (ac.have_heading && ac.kin_live && ac.maneuver != Maneuver::Land &&
        ac.maneuver != Maneuver::LineUp) {
      ac.hdg_deg = ac.target_heading;
    }
    if (ins.squawk) ac.squawk = *ins.squawk;
  }

  const std::string text = readbackText(ac, ins);
  // Read back on the current frequency before a contact instruction retunes.
  ac.client->sendText(ins.scope, ins.scope == osats::TextScope::Private ? ins.from : fmtFreq(ac.radio_hz),
                      text);
  std::cout << "readback " << ac.callsign << ": " << text << "\n";
  if (ins.contact_hz) {
    ac.radio_hz = *ins.contact_hz;
    ac.client->tuneRadio(ac.radio_hz);
  }
}

void drainInbox(Aircraft& ac) {
  if (!ac.inbox) return;
  std::vector<Instruction> batch;
  {
    std::lock_guard lock(ac.inbox->mu);
    batch.swap(ac.inbox->items);
  }
  for (const auto& ins : batch) {
    if (!ins.recognized && ac.inbox->said_again.count(ins.raw) != 0) continue;
    applyInstruction(ac, ins);
  }
}

void publish(Aircraft& ac) {
  const osats::Position pos = ac.kin_live ? fromKin(ac) : sampleScript(ac);
  ac.client->sendPosition(pos, kVisibilityNm);
  if (!ac.pattern && ac.pending_call != nullptr && ac.airborne_s >= kFirstCallDelayS) {
    const std::string text = ac.pending_call;
    ac.pending_call = nullptr;
    ac.client->sendText(osats::TextScope::Radio, "122.800", text);
    std::cout << "call " << ac.callsign << ": " << text << "\n";
  }
}

void usage() {
  std::cerr << "Usage: osats-traffic <host> <port> [seconds]\n"
            << "  Arrivals on 122.800 stay scripted. Pattern aircraft on 120.500 read back.\n"
            << "  Omit seconds to run until SIGINT or SIGTERM.\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string host = argv[1];
  const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
  if (port == 0) {
    usage();
    return 2;
  }
  const int seconds = argc >= 4 ? std::atoi(argv[3]) : -1;

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::cout << std::unitbuf;

  try {
    auto fleet = scriptedFleet();
    std::cerr << "osats-traffic: " << fleet.size() << " pilot sessions, KSFO\n";
    for (const auto& ac : fleet) {
      std::cerr << "  " << ac.callsign << " " << ac.plan.aircraft_type << " "
                << ac.plan.departure << "-" << ac.plan.arrival << " "
                << (ac.pattern ? "pattern 120.500" : "UNICOM 122.800") << "\n";
    }

    for (auto& ac : fleet) {
      if (ac.path.size() < 2) throw std::runtime_error(ac.callsign + " path needs two fixes");
      if (ac.pattern) ac.inbox = std::make_shared<Inbox>();
      ac.client = std::make_unique<osats::Client>();
      const std::string callsign = ac.callsign;
      ac.client->onError([callsign](osats::ErrorCode code, const std::string& msg) {
        std::cerr << callsign << " error " << static_cast<int>(code) << ": " << msg << "\n";
      });
      if (ac.pattern) {
        Aircraft* self = &ac;
        ac.client->onText([self](const osats::TextEvent& ev) {
          if (!forThisAircraft(*self, ev)) return;
          Instruction ins = parseInstruction(*self, ev);
          std::lock_guard lock(self->inbox->mu);
          self->inbox->items.push_back(std::move(ins));
        });
      }
      ac.client->connect(host, port, ac.callsign, osats::Role::Pilot, kOperatorName);
      ac.client->sendFlightPlan(ac.plan);
      ac.client->tuneRadio(ac.radio_hz);
      armLeg(ac);
      publish(ac);
    }

    std::ostringstream ready;
    ready << "ready";
    for (const auto& ac : fleet) {
      ready << ' ' << ac.callsign << '=' << ac.plan.departure << '-' << ac.plan.arrival << '/'
            << ac.plan.aircraft_type << '@' << ac.radio_hz;
    }
    std::cout << ready.str() << "\n";

    const auto started = std::chrono::steady_clock::now();
    while (!g_stop) {
      if (seconds >= 0 &&
          std::chrono::steady_clock::now() - started >= std::chrono::seconds(seconds)) {
        break;
      }
      std::this_thread::sleep_for(kTick);
      if (g_stop) break;
      const double dt = std::chrono::duration<double>(kTick).count();
      for (auto& ac : fleet) {
        if (!ac.client->connected()) throw std::runtime_error(ac.callsign + " disconnected");
        ac.airborne_s += dt;
        drainInbox(ac);
        if (ac.maneuver == Maneuver::Script) advanceScript(ac, dt);
        else stepManeuver(ac, dt);
        publish(ac);
      }
    }

    for (auto& ac : fleet) ac.client->disconnect();
    std::cerr << "osats-traffic: stopped\n";
  } catch (const std::exception& ex) {
    std::cerr << "fatal: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
