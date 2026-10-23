#include "osats/client.hpp"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// One automated tower for KSFO. A single controller session stands at the
// field, tuned to the published tower frequency, and issues a fixed set of
// text clearances from what interest shows: pilots on that frequency, and
// roughly where they are. It does not read replies, so injected aircraft are
// not required to comply.
//
// Usage: osats-tower <host> <port> [seconds]
// Omit seconds to run until SIGINT/SIGTERM.

namespace {

constexpr double kFieldLat = 37.618806;
constexpr double kFieldLon = -122.375417;
constexpr double kFieldAltFt = 13.0;
// KSFO ATCT, published 120.500 MHz. Wire unit is kHz (122.800 MHz is 122800).
constexpr std::uint32_t kTowerHz = 120500;
// Pattern plus the terminal arrivals this tower works.
constexpr double kVisibilityNm = 30.0;
constexpr const char* kCallsign = "KSFO_TWR";
constexpr const char* kOperatorName = "OSATS Tower";
constexpr auto kTick = std::chrono::milliseconds(200);
constexpr auto kPositionPeriod = std::chrono::seconds(1);
constexpr auto kTransmitGap = std::chrono::seconds(1);

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

enum class Clearance {
  HoldShort,
  LineUpAndWait,
  ClearedForTakeoff,
  ClearedToLand,
  RunwayHeading,
  EnterDownwind,
};

struct Offset {
  double north_nm = 0;
  double east_nm = 0;
  double dist_nm = 0;
};

Offset offsetFromField(const osats::Position& pos) {
  const double north = (pos.latitude_deg - kFieldLat) * 60.0;
  const double east = (pos.longitude_deg - kFieldLon) * 60.0 *
                      std::cos(kFieldLat * std::numbers::pi / 180.0);
  return {north, east, std::hypot(north, east)};
}

double headingDelta(double a, double b) {
  double d = std::fmod(std::abs(a - b), 360.0);
  if (d > 180.0) d = 360.0 - d;
  return d;
}

// Fixed phrase for each place a pilot on frequency can be. The callsign is
// the only piece taken from the peer.
std::optional<Clearance> classify(const osats::Position& pos) {
  const Offset at = offsetFromField(pos);
  const double alt = pos.altitude_ft;
  const double gs = pos.groundspeed_kt;
  const bool runway_heading = headingDelta(pos.heading_deg, 280.0) <= 40.0;

  if (alt < 150.0 && at.dist_nm < 0.8 && gs < 40.0) return Clearance::HoldShort;
  if (alt < 200.0 && at.dist_nm < 1.2 && gs >= 40.0 && runway_heading) {
    return Clearance::ClearedForTakeoff;
  }
  if (alt < 400.0 && at.dist_nm < 1.0 && gs < 30.0) return Clearance::LineUpAndWait;
  if (at.east_nm > 0.5 && at.north_nm < 2.0 && at.dist_nm > 1.0 && at.dist_nm < 8.0 &&
      alt < 4000.0 && runway_heading) {
    return Clearance::ClearedToLand;
  }
  if (at.east_nm < -1.0 && at.north_nm > 0.0 && at.dist_nm < 12.0 && alt >= 400.0 &&
      alt < 10000.0) {
    return Clearance::RunwayHeading;
  }
  if (at.dist_nm < 15.0 && alt < 8000.0) return Clearance::EnterDownwind;
  return std::nullopt;
}

std::string phrase(Clearance clearance, const std::string& callsign) {
  switch (clearance) {
    case Clearance::HoldShort:
      return callsign + ", at the field, hold short runway 28";
    case Clearance::LineUpAndWait:
      return callsign + ", runway 28, line up and wait";
    case Clearance::ClearedForTakeoff:
      return callsign + ", runway 28, cleared for takeoff";
    case Clearance::ClearedToLand:
      return callsign + ", runway 28, cleared to land";
    case Clearance::RunwayHeading:
      return callsign + ", fly runway heading, climb and maintain 3000";
    case Clearance::EnterDownwind:
      return callsign + ", enter left downwind runway 28";
  }
  return callsign + ", KSFO tower";
}

osats::Position fieldPosition() {
  osats::Position pos;
  pos.latitude_deg = kFieldLat;
  pos.longitude_deg = kFieldLon;
  pos.altitude_ft = kFieldAltFt;
  pos.groundspeed_kt = 0;
  pos.heading_deg = 280;
  pos.squawk = 0;
  pos.xpdr_mode = 'C';
  return pos;
}

void usage() {
  std::cerr << "Usage: osats-tower <host> <port> [seconds]\n"
            << "  Controller KSFO_TWR at the KSFO field on 120.500.\n"
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
    osats::Client tower;
    tower.onError([](osats::ErrorCode code, const std::string& msg) {
      std::cerr << "KSFO_TWR error " << static_cast<int>(code) << ": " << msg << "\n";
    });
    tower.connect(host, port, kCallsign, osats::Role::Controller, kOperatorName);
    tower.tuneRadio(kTowerHz);
    const osats::Position pos = fieldPosition();
    tower.sendPosition(pos, kVisibilityNm);

    std::cerr << "osats-tower: " << kCallsign << " (" << kOperatorName
              << ") at KSFO on 120.500, visibility " << kVisibilityNm << " nm\n";
    std::cout << "ready " << kCallsign << " " << kTowerHz << "\n";

    std::unordered_map<std::string, Clearance> issued;
    std::vector<std::pair<std::string, std::string>> queue;
    auto next_position = std::chrono::steady_clock::now() + kPositionPeriod;
    auto next_transmit = std::chrono::steady_clock::now();
    const auto started = std::chrono::steady_clock::now();

    while (!g_stop) {
      if (seconds >= 0 &&
          std::chrono::steady_clock::now() - started >= std::chrono::seconds(seconds)) {
        break;
      }
      if (!tower.connected()) throw std::runtime_error("tower disconnected");

      const auto now = std::chrono::steady_clock::now();
      if (now >= next_position) {
        tower.sendPosition(pos, kVisibilityNm);
        next_position = now + kPositionPeriod;
      }

      std::unordered_set<std::string> on_frequency;
      for (const auto& [id, peer] : tower.peers()) {
        (void)id;
        if (peer.role != osats::Role::Pilot) continue;
        if (peer.radio_hz != kTowerHz) continue;
        on_frequency.insert(peer.callsign);
        const auto clearance = classify(peer.position);
        if (!clearance) continue;
        const auto previous = issued.find(peer.callsign);
        if (previous != issued.end() && previous->second == *clearance) continue;
        issued[peer.callsign] = *clearance;
        queue.emplace_back(peer.callsign, phrase(*clearance, peer.callsign));
      }
      for (auto it = issued.begin(); it != issued.end();) {
        if (!on_frequency.count(it->first)) it = issued.erase(it);
        else ++it;
      }

      if (now >= next_transmit && !queue.empty()) {
        const auto [callsign, text] = queue.front();
        queue.erase(queue.begin());
        tower.sendText(osats::TextScope::Radio, "120.500", text);
        std::cout << "clearance " << callsign << ": " << text << "\n";
        next_transmit = now + kTransmitGap;
      }

      std::this_thread::sleep_for(kTick);
    }

    tower.disconnect();
    std::cerr << "osats-tower: stopped\n";
  } catch (const std::exception& ex) {
    std::cerr << "fatal: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
