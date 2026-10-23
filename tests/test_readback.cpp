#include "osats/client.hpp"
#include "osats/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Pattern aircraft read back and change the track they publish.
// osats-tower stops the two departures that are taxiing. A human pilot,
// parked north of the field on 120.500, then sends the rest of the set:
// squawk, heading, runway heading, climb, line up and wait, takeoff,
// one say-again, contact, plus a climb and a landing for the pattern airplane.
// UNICOM arrivals stay on 122.800 and keep their script.

namespace {

constexpr std::uint32_t kTowerHz = 120500;
constexpr std::uint32_t kUnicomHz = 122800;
constexpr double kFieldLat = 37.618806;
constexpr double kFieldLon = -122.375417;
// Inside the near interest tier, pure north so range is an honest 8 nm.
constexpr double kHumanNorthNm = 8.0;

void fail(const std::string& msg) { throw std::runtime_error(msg); }

std::uint16_t freePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) fail("socket");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    fail("bind probe");
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    ::close(fd);
    fail("getsockname");
  }
  const auto port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

template <typename Pred>
bool waitFor(Pred pred, int ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return pred();
}

double headingDelta(double a, double b) {
  double d = std::fmod(std::abs(a - b), 360.0);
  if (d > 180.0) d = 360.0 - d;
  return d;
}

struct Child {
  pid_t pid = -1;
  int read_fd = -1;
  std::thread reader;
  std::mutex mu;
  std::vector<std::string> lines;
  bool eof = false;

  ~Child() { stop(); }

  void stop() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      pid = -1;
    }
    if (reader.joinable()) reader.join();
    if (read_fd >= 0) {
      ::close(read_fd);
      read_fd = -1;
    }
  }

  std::vector<std::string> snapshot() {
    std::lock_guard lock(mu);
    return lines;
  }
};

void startProcess(Child& child, const std::string& bin, std::uint16_t port) {
  int pipefd[2];
  if (::pipe(pipefd) < 0) fail("pipe");
  const std::string port_str = std::to_string(port);
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    fail("fork");
  }
  if (pid == 0) {
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    for (int fd = 3; fd < 256; ++fd) ::close(fd);
    ::execl(bin.c_str(), bin.c_str(), "127.0.0.1", port_str.c_str(),
            static_cast<char*>(nullptr));
    const char msg[] = "exec failed\n";
    if (::write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0) {
    }
    _exit(127);
  }
  ::close(pipefd[1]);
  child.pid = pid;
  child.read_fd = pipefd[0];
  child.reader = std::thread([&child] {
    std::string pending;
    char buf[512];
    while (true) {
      const ssize_t n = ::read(child.read_fd, buf, sizeof(buf));
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) break;
      pending.append(buf, buf + n);
      std::size_t nl = 0;
      while ((nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pending.erase(0, nl + 1);
        std::lock_guard lock(child.mu);
        child.lines.push_back(std::move(line));
      }
    }
    if (!pending.empty()) {
      std::lock_guard lock(child.mu);
      child.lines.push_back(std::move(pending));
    }
    std::lock_guard lock(child.mu);
    child.eof = true;
  });
}

bool hasReady(Child& child, const std::string& prefix) {
  for (const auto& line : child.snapshot()) {
    if (line.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: test_readback <osats-traffic-bin> <osats-tower-bin>\n";
    return 2;
  }
  const std::string traffic_bin = argv[1];
  const std::string tower_bin = argv[2];

  try {
    const auto port = freePort();
    osats::ServerConfig config;
    config.port = port;
    config.enable_http = false;
    config.verbose = false;
    osats::Server server(config);
    std::thread server_thread([&server] {
      try {
        server.run();
      } catch (const std::exception& ex) {
        std::cerr << "server: " << ex.what() << "\n";
      }
    });
    struct JoinServer {
      osats::Server& server;
      std::thread& thread;
      ~JoinServer() {
        server.requestStop();
        if (thread.joinable()) thread.join();
      }
    } join{server, server_thread};

    osats::Client human;
    std::mutex mu;
    std::unordered_map<std::string, osats::Peer> seen;
    std::vector<osats::TextEvent> texts;
    std::vector<std::string> errors;

    human.onPeerUpdate([&](const osats::Peer& peer) {
      std::lock_guard lock(mu);
      seen[peer.callsign] = peer;
    });
    human.onText([&](const osats::TextEvent& ev) {
      std::lock_guard lock(mu);
      texts.push_back(ev);
    });
    human.onError([&](osats::ErrorCode code, const std::string& msg) {
      std::lock_guard lock(mu);
      errors.push_back(std::to_string(static_cast<int>(code)) + " " + msg);
    });

    human.connect("127.0.0.1", port, "N17XX", osats::Role::Pilot, "Ada Lovelace");
    human.tuneRadio(kTowerHz);
    osats::Position self;
    self.latitude_deg = kFieldLat + kHumanNorthNm / 60.0;
    self.longitude_deg = kFieldLon;
    self.altitude_ft = 2500;
    self.heading_deg = 180;
    self.squawk = 1200;
    self.xpdr_mode = 'C';
    human.sendPosition(self, 50.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto peerOf = [&](const std::string& callsign) -> std::optional<osats::Peer> {
      std::lock_guard lock(mu);
      const auto it = seen.find(callsign);
      if (it == seen.end()) return std::nullopt;
      return it->second;
    };
    auto heard = [&](const std::string& from, osats::TextScope scope, const std::string& fragment) {
      std::lock_guard lock(mu);
      for (const auto& ev : texts) {
        if (ev.from == from && ev.scope == scope && ev.text.find(fragment) != std::string::npos) {
          return true;
        }
      }
      return false;
    };
    auto sayAgainCount = [&](const std::string& from) {
      std::lock_guard lock(mu);
      int n = 0;
      for (const auto& ev : texts) {
        if (ev.from == from && ev.text.find("say again") != std::string::npos) ++n;
      }
      return n;
    };
    auto detail = [&] {
      std::lock_guard lock(mu);
      std::ostringstream oss;
      if (!errors.empty()) {
        oss << " errors:";
        for (const auto& err : errors) oss << " " << err;
      }
      oss << " texts:";
      for (const auto& ev : texts) oss << " [" << ev.from << ": " << ev.text << "]";
      return oss.str();
    };

    Child traffic;
    startProcess(traffic, traffic_bin, port);
    if (!waitFor([&] { return hasReady(traffic, "ready "); }, 5000)) {
      fail("timed out waiting for traffic");
    }

    double swa_max_gs = 0;
    if (!waitFor(
            [&] {
              auto swa = peerOf("SWA2104");
              if (!swa) return false;
              swa_max_gs = std::max(swa_max_gs, swa->position.groundspeed_kt);
              return swa_max_gs >= 10.0;
            },
            3000)) {
      fail("SWA2104 was not taxiing before the tower connected" + detail());
    }

    osats::Position aal_at{};
    osats::Position ual_at{};
    if (!waitFor(
            [&] {
              auto aal = peerOf("AAL142");
              auto ual = peerOf("UAL482");
              if (!aal || !ual) return false;
              if (aal->radio_hz != kUnicomHz || ual->radio_hz != kUnicomHz) return false;
              aal_at = aal->position;
              ual_at = ual->position;
              return true;
            },
            3000)) {
      fail("UNICOM arrivals were not on 122800" + detail());
    }

    Child tower;
    startProcess(tower, tower_bin, port);
    if (!waitFor([&] { return hasReady(tower, "ready KSFO_TWR "); }, 5000)) {
      fail("timed out waiting for tower");
    }

    auto towerHold = [&](const std::string& callsign) {
      std::lock_guard lock(mu);
      for (const auto& ev : texts) {
        if (ev.from == "KSFO_TWR" && ev.scope == osats::TextScope::Radio &&
            ev.text.find(callsign) != std::string::npos &&
            ev.text.find("hold short") != std::string::npos) {
          return true;
        }
      }
      return false;
    };
    auto holding = [&](const std::string& callsign) {
      auto peer = peerOf(callsign);
      if (!peer) return false;
      if (callsign == "SWA2104") {
        swa_max_gs = std::max(swa_max_gs, peer->position.groundspeed_kt);
      }
      return towerHold(callsign) && heard(callsign, osats::TextScope::Radio, "holding short") &&
             peer->position.groundspeed_kt <= 1.0;
    };
    // Both departures must be stopped before the human sends a lineup,
    // or a later tower hold-short would cancel it.
    if (!waitFor([&] { return holding("SWA2104") && holding("DAL415"); }, 12000)) {
      fail("tower hold short did not stop the departures" + detail());
    }
    if (swa_max_gs < 10.0) fail("SWA2104 never showed a taxi speed");

    const auto swa_held = *peerOf("SWA2104");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const auto swa_still = *peerOf("SWA2104");
    const double held_dlat =
        std::abs(swa_still.position.latitude_deg - swa_held.position.latitude_deg);
    const double held_dlon =
        std::abs(swa_still.position.longitude_deg - swa_held.position.longitude_deg);
    if (held_dlat > 0.00005 || held_dlon > 0.00005 || swa_still.position.groundspeed_kt > 1.0) {
      fail("SWA2104 moved while holding short");
    }
    std::cout << "tower SWA2104 hold short, gs " << swa_max_gs << " -> "
              << swa_still.position.groundspeed_kt << "\n";
    std::cout << "tower DAL415 hold short, gs " << peerOf("DAL415")->position.groundspeed_kt
              << "\n";

    const auto swa_squawk_before = swa_still.position.squawk;
    human.sendText(osats::TextScope::Private, "SWA2104", "squawk 4261");
    if (!waitFor(
            [&] {
              auto swa = peerOf("SWA2104");
              return swa && swa->position.squawk == 4261 &&
                     heard("SWA2104", osats::TextScope::Private, "squawk 4261");
            },
            5000)) {
      fail("SWA2104 did not copy squawk 4261" + detail());
    }
    std::cout << "human SWA2104 squawk " << swa_squawk_before << " -> 4261\n";

    human.sendText(osats::TextScope::Radio, "120.500", "DAL415 fly heading 040");
    if (!waitFor(
            [&] {
              auto dal = peerOf("DAL415");
              return dal && dal->position.groundspeed_kt <= 1.0 &&
                     headingDelta(dal->position.heading_deg, 40.0) < 2.0 &&
                     heard("DAL415", osats::TextScope::Radio, "heading 40");
            },
            5000)) {
      fail("DAL415 did not copy heading 040" + detail());
    }
    std::cout << "human DAL415 heading " << peerOf("DAL415")->position.heading_deg << "\n";

    human.sendText(osats::TextScope::Radio, "120.500", "DAL415 fly runway heading");
    if (!waitFor(
            [&] {
              auto dal = peerOf("DAL415");
              return dal && dal->position.groundspeed_kt <= 1.0 &&
                     headingDelta(dal->position.heading_deg, 280.0) < 2.0 &&
                     heard("DAL415", osats::TextScope::Radio, "runway heading");
            },
            5000)) {
      fail("DAL415 did not copy runway heading" + detail());
    }
    std::cout << "human DAL415 runway heading " << peerOf("DAL415")->position.heading_deg << "\n";

    const double dal_hold_alt = peerOf("DAL415")->position.altitude_ft;
    human.sendText(osats::TextScope::Radio, "120.500", "DAL415 climb and maintain 6000");
    if (!waitFor(
            [&] {
              auto dal = peerOf("DAL415");
              return dal && dal->position.groundspeed_kt <= 1.0 &&
                     heard("DAL415", osats::TextScope::Radio, "climbing to 6000");
            },
            5000)) {
      fail("DAL415 did not read back the climb" + detail());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double dal_after_climb = peerOf("DAL415")->position.altitude_ft;
    if (dal_after_climb > dal_hold_alt + 30.0) {
      fail("DAL415 climbed while holding short");
    }
    std::cout << "human DAL415 climb assigned, still holding, alt " << dal_after_climb << "\n";

    const auto lineup_from = *peerOf("DAL415");
    human.sendText(osats::TextScope::Radio, "120.500", "DAL415, runway 28, line up and wait");
    if (!waitFor(
            [&] {
              auto dal = peerOf("DAL415");
              if (!dal) return false;
              const double dlat =
                  dal->position.latitude_deg - lineup_from.position.latitude_deg;
              const double dlon =
                  dal->position.longitude_deg - lineup_from.position.longitude_deg;
              const double moved = std::hypot(dlat, dlon);
              return heard("DAL415", osats::TextScope::Radio, "lining up") &&
                     dal->position.groundspeed_kt <= 1.0 && moved > 0.0002 &&
                     headingDelta(dal->position.heading_deg, 280.0) < 5.0;
            },
            15000)) {
      fail("DAL415 did not line up and wait" + detail());
    }
    const auto lined_up = *peerOf("DAL415");
    std::cout << "human DAL415 lined up, hdg " << lined_up.position.heading_deg << " at "
              << lined_up.position.latitude_deg << "," << lined_up.position.longitude_deg << "\n";

    const double takeoff_alt = lined_up.position.altitude_ft;
    human.sendText(osats::TextScope::Radio, "120.500", "DAL415, runway 28, cleared for takeoff");
    if (!waitFor(
            [&] {
              auto dal = peerOf("DAL415");
              return dal && heard("DAL415", osats::TextScope::Radio, "cleared for takeoff") &&
                     dal->position.groundspeed_kt >= 40.0 &&
                     dal->position.altitude_ft >= takeoff_alt + 40.0;
            },
            8000)) {
      fail("DAL415 did not take off toward the assigned altitude" + detail());
    }
    const auto airborne = *peerOf("DAL415");
    std::cout << "human DAL415 takeoff gs " << airborne.position.groundspeed_kt << " alt "
              << airborne.position.altitude_ft << "\n";

    const int say_before = sayAgainCount("DAL415");
    human.sendText(osats::TextScope::Radio, "120.500", "DAL415, go around the moon");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    human.sendText(osats::TextScope::Radio, "120.500", "DAL415, go around the moon");
    if (!waitFor([&] { return sayAgainCount("DAL415") >= say_before + 1; }, 3000)) {
      fail("DAL415 did not say again" + detail());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const int say_after = sayAgainCount("DAL415");
    const auto after_garbage = *peerOf("DAL415");
    if (say_after != say_before + 1) {
      fail("DAL415 said again " + std::to_string(say_after - say_before) + " times");
    }
    if (after_garbage.position.groundspeed_kt < 40.0) {
      fail("DAL415 left the takeoff after say again");
    }
    std::cout << "human DAL415 say again once, gs " << after_garbage.position.groundspeed_kt
              << "\n";

    human.sendText(osats::TextScope::Radio, "120.500", "DAL415 contact departure 124.900");
    if (!waitFor(
            [&] {
              auto dal = peerOf("DAL415");
              return dal && dal->radio_hz == 124900 &&
                     heard("DAL415", osats::TextScope::Radio, "124.900");
            },
            5000)) {
      fail("DAL415 did not read back and retune to 124.900" + detail());
    }
    std::cout << "human DAL415 contact 124.900, radio " << peerOf("DAL415")->radio_hz
              << " readback heard on 120.500\n";

    const double n172_alt = peerOf("N172SP")->position.altitude_ft;
    human.sendText(osats::TextScope::Radio, "120.500", "N172SP climb and maintain 4000");
    if (!waitFor(
            [&] {
              auto n172 = peerOf("N172SP");
              return n172 && heard("N172SP", osats::TextScope::Radio, "climbing to 4000") &&
                     n172->position.altitude_ft >= n172_alt + 40.0;
            },
            8000)) {
      fail("N172SP did not climb" + detail());
    }
    const double n172_climbed = peerOf("N172SP")->position.altitude_ft;
    std::cout << "human N172SP climb " << n172_alt << " -> " << n172_climbed << "\n";

    const auto n172_before_land = *peerOf("N172SP");
    human.sendText(osats::TextScope::Radio, "120.500", "N172SP cleared to land runway 28");
    if (!waitFor(
            [&] {
              auto n172 = peerOf("N172SP");
              if (!n172) return false;
              return heard("N172SP", osats::TextScope::Radio, "cleared to land") &&
                     n172->position.altitude_ft <= n172_before_land.position.altitude_ft - 30.0 &&
                     n172->position.latitude_deg <
                         n172_before_land.position.latitude_deg - 0.0003;
            },
            8000)) {
      fail("N172SP did not turn back toward the field and descend" + detail());
    }
    const auto n172_landing = *peerOf("N172SP");
    std::cout << "human N172SP land alt " << n172_before_land.position.altitude_ft << " -> "
              << n172_landing.position.altitude_ft << " lat "
              << n172_before_land.position.latitude_deg << " -> "
              << n172_landing.position.latitude_deg << "\n";

    const auto aal = *peerOf("AAL142");
    const auto ual = *peerOf("UAL482");
    const bool aal_moved =
        std::abs(aal.position.latitude_deg - aal_at.latitude_deg) > 1e-4 ||
        std::abs(aal.position.longitude_deg - aal_at.longitude_deg) > 1e-4;
    const bool ual_moved =
        std::abs(ual.position.latitude_deg - ual_at.latitude_deg) > 1e-4 ||
        std::abs(ual.position.longitude_deg - ual_at.longitude_deg) > 1e-4;
    if (aal.radio_hz != kUnicomHz || ual.radio_hz != kUnicomHz || !aal_moved || !ual_moved) {
      fail("UNICOM arrivals left their script");
    }
    std::cout << "arrivals AAL142@" << aal.radio_hz << " UAL482@" << ual.radio_hz
              << " still scripted\n";
    std::cout << "test_readback ok\n";

    tower.stop();
    traffic.stop();
    human.disconnect();
  } catch (const std::exception& ex) {
    std::cerr << "test_readback failed: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
