#include "osats/client.hpp"
#include "osats/geo.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct AppState {
  std::mutex mu;
  std::deque<std::string> log;
  osats::Position pos;
  bool moving = false;
  double move_hdg = 90;
  double move_gs = 0;
  std::uint32_t radio_hz = 122800;
};

void pushLog(AppState& st, std::string line) {
  std::lock_guard lock(st.mu);
  st.log.push_back(std::move(line));
  while (st.log.size() > 12) st.log.pop_front();
}

std::string fmtFreq(std::uint32_t hz) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", hz / 1000.0);
  return buf;
}

void printHelp() {
  std::cout <<
      R"(OSATS interactive client — commands:
  connect <host> <port> <callsign> [role] [name]
      role = pilot|observer|controller (default pilot)
  pos <lat> <lon> [alt] [gs] [hdg] [squawk]
  move <hdg> <gs>          auto-advance position each second
  stop                     stop auto-move
  radio <freq>             e.g. 122.8
  say <callsign> <text>    private message
  radio-say <text>         transmit on tuned frequency
  wallop <text>            message controllers
  fp <dep> <arr> <acft> [rules] [cruise] [route...]
  peers                    list nearby traffic
  info <callsign>
  plan <callsign>         query a filed flight plan
  metar <icao>
  atis <icao>
  ping
  status
  help
  quit
)";
}

osats::Role parseRole(const std::string& s) {
  if (s == "controller" || s == "atc") return osats::Role::Controller;
  if (s == "observer" || s == "obs") return osats::Role::Observer;
  return osats::Role::Pilot;
}

void render(const osats::Client& client, AppState& st) {
  std::cout << "\n\033[36m── OSATS ─────────────────────────────────────────────\033[0m\n";
  if (!client.connected()) {
    std::cout << "  not connected — type: connect 127.0.0.1 4400 N12345\n";
  } else {
    std::cout << "  " << client.serverName() << "  id=" << client.entityId()
              << "  radio=" << fmtFreq(st.radio_hz) << " MHz\n";
    std::cout << "  pos " << st.pos.latitude_deg << ", " << st.pos.longitude_deg
              << "  alt " << st.pos.altitude_ft << "  "
              << st.pos.groundspeed_kt << "kt / " << st.pos.heading_deg << "°"
              << (st.moving ? "  [moving]" : "") << "\n";
  }

  auto peers = client.peers();
  std::cout << "\033[33m  traffic (" << peers.size() << ")\033[0m\n";
  for (const auto& [id, p] : peers) {
    const double dist = osats::geo::distanceNm(st.pos, p.position);
    std::cout << "    " << p.callsign << "  " << osats::roleName(p.role)
              << "  " << static_cast<int>(p.position.altitude_ft) << "ft  "
              << static_cast<int>(p.position.groundspeed_kt) << "kt  "
              << static_cast<int>(dist) << "nm";
    if (p.flight_plan) {
      std::cout << "  " << p.flight_plan->departure << "→" << p.flight_plan->arrival;
    }
    std::cout << "\n";
  }

  {
    std::lock_guard lock(st.mu);
    if (!st.log.empty()) {
      std::cout << "\033[32m  messages\033[0m\n";
      for (const auto& line : st.log) std::cout << "    " << line << "\n";
    }
  }
  std::cout << "\033[36m──────────────────────────────────────────────────────\033[0m\n";
  std::cout << "osats> " << std::flush;
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  printHelp();

  AppState st;
  st.pos.latitude_deg = 37.6213;
  st.pos.longitude_deg = -122.3790;
  st.pos.altitude_ft = 0;
  st.pos.squawk = 1200;
  st.pos.xpdr_mode = 'C';

  osats::Client client;
  client.onAnnounce([&](const std::string& name, const std::string& ver) {
    pushLog(st, "server announce: " + name + " " + ver);
  });
  client.onText([&](const osats::TextEvent& ev) {
    pushLog(st, "[" + ev.from + "] " + ev.text);
  });
  client.onMetar([&](const std::string& icao, const std::string& metar) {
    pushLog(st, "METAR " + icao + ": " + metar);
  });
  client.onReply([&](osats::QueryType, const std::string& target, const std::string& body) {
    pushLog(st, "reply " + target + ": " + body);
  });
  client.onError([&](osats::ErrorCode code, const std::string& msg) {
    pushLog(st, "error " + std::to_string(static_cast<int>(code)) + ": " + msg);
  });
  client.onPeerUpdate([&](const osats::Peer& p) {
    pushLog(st, "traffic: " + p.callsign);
  });

  std::atomic<bool> alive{true};
  std::thread ticker([&] {
    while (alive.load()) {
      if (client.connected()) {
        try {
          if (st.moving) {
            // Advance ~gs knots along heading for 1 second.
            const double nm = st.move_gs / 3600.0;
            const double hdg = st.move_hdg * M_PI / 180.0;
            const double dlat = (nm / 60.0) * std::cos(hdg);
            const double dlon =
                (nm / 60.0) * std::sin(hdg) /
                std::max(0.2, std::cos(st.pos.latitude_deg * M_PI / 180.0));
            st.pos.latitude_deg += dlat;
            st.pos.longitude_deg += dlon;
            st.pos.heading_deg = st.move_hdg;
            st.pos.groundspeed_kt = st.move_gs;
            const double fps = st.move_gs * 1.68781;
            st.pos.vn_fps = static_cast<float>(fps * std::cos(hdg));
            st.pos.ve_fps = static_cast<float>(fps * std::sin(hdg));
          }
          client.sendPosition(st.pos);
          client.sendHeartbeat();
        } catch (...) {
        }
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });

  // Optional auto-connect from argv: osats-cli host port callsign
  if (argc >= 4) {
    try {
      client.connect(argv[1], static_cast<std::uint16_t>(std::atoi(argv[2])), argv[3]);
      client.tuneRadio(st.radio_hz);
      client.sendPosition(st.pos);
      pushLog(st, "connected as " + std::string(argv[3]));
    } catch (const std::exception& ex) {
      std::cerr << "auto-connect failed: " << ex.what() << "\n";
    }
  }

  render(client, st);
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    try {
      if (cmd.empty()) {
        // refresh
      } else if (cmd == "help" || cmd == "?") {
        printHelp();
      } else if (cmd == "quit" || cmd == "exit") {
        break;
      } else if (cmd == "connect") {
        std::string host, callsign, role = "pilot", name;
        int port = 4400;
        iss >> host >> port >> callsign >> role >> name;
        if (host.empty() || callsign.empty()) {
          std::cout << "usage: connect <host> <port> <callsign> [role] [name]\n";
        } else {
          if (client.connected()) client.disconnect();
          client.connect(host, static_cast<std::uint16_t>(port), callsign,
                         parseRole(role), name);
          client.tuneRadio(st.radio_hz);
          client.sendPosition(st.pos);
          pushLog(st, "connected as " + callsign + " @ " + client.serverName());
        }
      } else if (cmd == "pos") {
        iss >> st.pos.latitude_deg >> st.pos.longitude_deg;
        double alt = st.pos.altitude_ft, gs = st.pos.groundspeed_kt,
               hdg = st.pos.heading_deg;
        int squawk = st.pos.squawk;
        if (iss >> alt) st.pos.altitude_ft = alt;
        if (iss >> gs) st.pos.groundspeed_kt = gs;
        if (iss >> hdg) st.pos.heading_deg = hdg;
        if (iss >> squawk) st.pos.squawk = static_cast<std::uint16_t>(squawk);
        if (client.connected()) client.sendPosition(st.pos);
      } else if (cmd == "move") {
        iss >> st.move_hdg >> st.move_gs;
        st.moving = true;
        st.pos.heading_deg = st.move_hdg;
        st.pos.groundspeed_kt = st.move_gs;
      } else if (cmd == "stop") {
        st.moving = false;
        st.move_gs = 0;
        st.pos.groundspeed_kt = 0;
      } else if (cmd == "radio") {
        double f = 122.8;
        iss >> f;
        st.radio_hz = static_cast<std::uint32_t>(std::lround(f * 1000.0));
        if (client.connected()) client.tuneRadio(st.radio_hz);
      } else if (cmd == "say") {
        std::string to;
        iss >> to;
        std::string text;
        std::getline(iss, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        client.sendText(osats::TextScope::Private, to, text);
      } else if (cmd == "radio-say") {
        std::string text;
        std::getline(iss, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        client.sendText(osats::TextScope::Radio, fmtFreq(st.radio_hz), text);
      } else if (cmd == "wallop") {
        std::string text;
        std::getline(iss, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        client.sendText(osats::TextScope::Wallop, "ATC", text);
      } else if (cmd == "fp") {
        osats::FlightPlan fp;
        iss >> fp.departure >> fp.arrival >> fp.aircraft_type;
        std::string rules = "I";
        int cruise = 35000;
        if (iss >> rules) fp.flight_rules = rules.empty() ? 'I' : rules[0];
        if (iss >> cruise) fp.cruise_alt_ft = static_cast<std::uint16_t>(cruise);
        std::string route;
        std::getline(iss, route);
        if (!route.empty() && route[0] == ' ') route.erase(0, 1);
        fp.route = route.empty() ? "DCT" : route;
        fp.tas_kt = 450;
        client.sendFlightPlan(fp);
        pushLog(st, "filed " + fp.departure + "→" + fp.arrival);
      } else if (cmd == "peers" || cmd == "status") {
        // rendered below
      } else if (cmd == "info") {
        std::string cs;
        iss >> cs;
        client.query(osats::QueryType::Info, cs);
      } else if (cmd == "plan") {
        std::string cs;
        iss >> cs;
        if (cs.empty()) {
          std::cout << "usage: plan <callsign>\n";
        } else {
          client.query(osats::QueryType::FlightPlan, cs);
        }
      } else if (cmd == "metar") {
        std::string icao;
        iss >> icao;
        client.requestMetar(icao);
      } else if (cmd == "atis") {
        std::string icao;
        iss >> icao;
        client.query(osats::QueryType::ATIS, icao);
      } else if (cmd == "ping") {
        client.ping(static_cast<std::uint32_t>(::getpid()));
        pushLog(st, "ping sent");
      } else {
        std::cout << "unknown command — type help\n";
      }
    } catch (const std::exception& ex) {
      std::cout << "error: " << ex.what() << "\n";
    }
    // small delay so async replies can land in the log
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    render(client, st);
  }

  alive.store(false);
  ticker.join();
  client.disconnect();
  return 0;
}
