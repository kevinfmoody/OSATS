#include "osats/client.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Open-loop injected pilots for one field (KSFO) on UNICOM (122.800).
// One Client session per aircraft. Paths, plans, and position calls are a
// timetable: nothing in here reads peer state or radio replies.
//
// Usage: osats-traffic <host> <port> [seconds]
// Omit seconds to run until SIGINT/SIGTERM.

namespace {

constexpr double kFieldLat = 37.618806;
constexpr double kFieldLon = -122.375417;
constexpr std::uint32_t kUnicomHz = 122800;
// First call waits until the session has been in the air long enough for a
// pilot who connected first to be inside the spatial index.
constexpr double kFirstCallDelayS = 0.5;
constexpr double kVisibilityNm = 40.0;
constexpr const char* kOperatorName = "OSATS Traffic";
constexpr auto kTick = std::chrono::milliseconds(200);

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

struct Fix {
  double north_nm = 0;
  double east_nm = 0;
  double altitude_ft = 0;
  double groundspeed_kt = 0;
  const char* call = nullptr;
};

osats::FlightPlan makePlan(char rules, const char* type, const char* dep,
                           const char* arr, std::uint16_t cruise_ft,
                           std::uint16_t tas_kt, const char* route) {
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

struct Aircraft {
  std::string callsign;
  std::uint16_t squawk = 1200;
  osats::FlightPlan plan;
  std::vector<Fix> path;
  std::unique_ptr<osats::Client> client;
  std::size_t leg = 0;
  double leg_elapsed_s = 0;
  double leg_duration_s = 1;
  double airborne_s = 0;
  const char* pending_call = nullptr;
};

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
    Aircraft ac;
    ac.callsign = "SWA2104";
    ac.squawk = 4203;
    ac.plan = makePlan('I', "B738", "KSFO", "KLAX", 36000, 450, "SSTIK3 PORTE");
    ac.path = {
        {0.1, 0.4, 50, 150,
         "KSFO traffic, Southwest 2104, departing runway 28, climbing northwest, Southwest 2104"},
        {1.5, -4.0, 2500, 190, nullptr},
        {4.0, -10.0, 7000, 250, nullptr},
        {10.0, -12.0, 12000, 280,
         "KSFO traffic, Southwest 2104, 12 miles northwest, climbing, Southwest 2104"},
        {14.0, -4.0, 15000, 300, nullptr},
        {12.0, 8.0, 16000, 300, nullptr},
        {4.0, 10.0, 8000, 250, nullptr},
        {0.5, 3.0, 2000, 180, nullptr},
    };
    fleet.push_back(std::move(ac));
  }
  {
    Aircraft ac;
    ac.callsign = "DAL415";
    ac.squawk = 4204;
    ac.plan = makePlan('I', "B739", "KSFO", "KATL", 38000, 460, "SSTIK3 WAGES");
    ac.path = {
        {0.0, -0.2, 40, 150,
         "KSFO traffic, Delta 415, departing runway 28, climbing west, Delta 415"},
        {1.0, -6.0, 4000, 210, nullptr},
        {-4.0, -10.0, 10000, 270, nullptr},
        {-12.0, -8.0, 18000, 300,
         "KSFO traffic, Delta 415, south of the field, climbing, Delta 415"},
        {-12.0, 0.0, 20000, 300, nullptr},
        {-6.0, 8.0, 14000, 280, nullptr},
        {0.0, 6.0, 6000, 220, nullptr},
        {0.2, 1.5, 1500, 170, nullptr},
    };
    fleet.push_back(std::move(ac));
  }
  {
    Aircraft ac;
    ac.callsign = "N172SP";
    ac.squawk = 1200;
    ac.plan = makePlan('V', "C172", "KSFO", "KOAK", 2500, 110, "VFR BAY");
    ac.path = {
        {2.0, 4.0, 1500, 90,
         "KSFO traffic, Skyhawk 172SP, over the bay, 1500, northbound, Skyhawk"},
        {6.0, 5.0, 1500, 90,
         "KSFO traffic, Skyhawk 172SP, over the bay, 1500, eastbound, Skyhawk"},
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
  const double dist =
      std::hypot(to.north_nm - from.north_nm, to.east_nm - from.east_nm);
  const double gs = std::max(from.groundspeed_kt, 30.0);
  ac.leg_duration_s = std::max(dist / gs * 3600.0, 0.5);
  ac.pending_call = from.call;
}

osats::Position sample(const Aircraft& ac) {
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
  pos.longitude_deg =
      kFieldLon + east / (60.0 * std::cos(kFieldLat * std::numbers::pi / 180.0));
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

void advance(Aircraft& ac, double dt) {
  ac.leg_elapsed_s += dt;
  while (ac.leg_elapsed_s >= ac.leg_duration_s) {
    ac.leg_elapsed_s -= ac.leg_duration_s;
    ac.leg = (ac.leg + 1) % ac.path.size();
    armLeg(ac);
  }
}

void publish(Aircraft& ac) {
  ac.client->sendPosition(sample(ac), kVisibilityNm);
  if (ac.pending_call != nullptr && ac.airborne_s >= kFirstCallDelayS) {
    const std::string text = ac.pending_call;
    ac.pending_call = nullptr;
    ac.client->sendText(osats::TextScope::Radio, "122.800", text);
    std::cout << "call " << ac.callsign << ": " << text << "\n";
  }
}

void usage() {
  std::cerr << "Usage: osats-traffic <host> <port> [seconds]\n"
            << "  One pilot session per scripted KSFO aircraft, tuned to 122.800.\n"
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
    std::cerr << "osats-traffic: " << fleet.size()
              << " pilot sessions, KSFO, 122.800, open-loop\n";
    for (const auto& ac : fleet) {
      std::cerr << "  " << ac.callsign << " " << ac.plan.aircraft_type << " "
                << ac.plan.departure << "-" << ac.plan.arrival << "\n";
    }

    for (auto& ac : fleet) {
      if (ac.path.size() < 2) {
        throw std::runtime_error(ac.callsign + " path needs two fixes");
      }
      ac.client = std::make_unique<osats::Client>();
      const std::string callsign = ac.callsign;
      ac.client->onError([callsign](osats::ErrorCode code, const std::string& msg) {
        std::cerr << callsign << " error " << static_cast<int>(code) << ": " << msg
                  << "\n";
      });
      ac.client->connect(host, port, ac.callsign, osats::Role::Pilot, kOperatorName);
      ac.client->sendFlightPlan(ac.plan);
      ac.client->tuneRadio(kUnicomHz);
      armLeg(ac);
      publish(ac);
    }

    std::ostringstream ready;
    ready << "ready";
    for (const auto& ac : fleet) {
      ready << ' ' << ac.callsign << '=' << ac.plan.departure << '-' << ac.plan.arrival
            << '/' << ac.plan.aircraft_type;
    }
    std::cout << ready.str() << "\n";

    const auto started = std::chrono::steady_clock::now();
    while (!g_stop) {
      if (seconds >= 0) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed >= std::chrono::seconds(seconds)) break;
      }
      std::this_thread::sleep_for(kTick);
      if (g_stop) break;
      const double dt = std::chrono::duration<double>(kTick).count();
      for (auto& ac : fleet) {
        if (!ac.client->connected()) {
          throw std::runtime_error(ac.callsign + " disconnected");
        }
        ac.airborne_s += dt;
        advance(ac, dt);
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
